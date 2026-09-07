// NamEngine.cpp — see NamEngine.h.
//
// MIT license, (c) 2026 SuprPedals contributors.

#include "NamEngine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>

#include <pthread.h>
#include <sched.h>

namespace supr {

void tryElevateThreadPriority(const char* name)
{
#ifdef __linux__
    // Naming the thread first, because it costs nothing and it is the only
    // way anyone will ever identify these in `ps`, `chrt` or a profiler. TooB
    // NAM's background thread shows up as tnam_bg, which is how the settings
    // below were worked out in the first place. Linux caps this at 15 chars.
    if (name)
        pthread_setname_np(pthread_self(), name);

    // SCHED_RR at 75, matching what PiPedal's own plugin threads use.
    //
    // Measured on a live pipedald: the ALSA audio thread runs SCHED_RR 90,
    // PiPedal's realtime service thread 85, and TooB NAM's background worker
    // 75. Sitting at 75 puts these threads above every housekeeping thread in
    // the process — so a model is never descheduled behind a web request or a
    // dbus callback — while leaving a 15-priority gap beneath the thread they
    // are feeding, which is what stops a busy worker from ever starving the
    // audio thread it exists to serve.
    //
    // RR rather than FIFO because a parallel stage runs up to three helpers at
    // the *same* priority; with FIFO, one that failed to yield could hold a
    // core indefinitely against its own siblings.
    //
    // pipedald runs unprivileged but is granted RLIMIT_RTPRIO 95 by its unit
    // file, so this succeeds there. It fails, silently and harmlessly, when
    // the tests run it from an ordinary login shell.
    constexpr int kWorkerPriority = 75;
    const int maxPriority = sched_get_priority_max(SCHED_RR);
    if (maxPriority <= 0)
        return;
    sched_param param{};
    param.sched_priority = std::min(kWorkerPriority, maxPriority);
    pthread_setschedparam(pthread_self(), SCHED_RR, &param);
#else
    (void)name;
#endif
}

// ---------------------------------------------------------------------------
// NamWorkerPool
// ---------------------------------------------------------------------------

void NamWorkerPool::start(int helpers)
{
    if (helpers == static_cast<int>(threads_.size()))
        return;
    stop();
    if (helpers <= 0)
        return;

    quit_.store(false, std::memory_order_relaxed);
    jobs_.assign(static_cast<size_t>(helpers), Job{});
    wake_.clear();
    wake_.reserve(static_cast<size_t>(helpers));
    for (int i = 0; i < helpers; ++i)
        wake_.push_back(std::make_unique<Semaphore>());

    threads_.reserve(static_cast<size_t>(helpers));
    for (int i = 0; i < helpers; ++i)
        threads_.emplace_back([this, i] { helperLoop(i); });
}

void NamWorkerPool::stop()
{
    if (threads_.empty())
        return;
    quit_.store(true, std::memory_order_release);
    for (auto& sem : wake_)
        sem->post();
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
    wake_.clear();
    jobs_.clear();
}

void NamWorkerPool::helperLoop(int index)
{
    char name[16];
    std::snprintf(name, sizeof(name), "snam_w%d", index);
    tryElevateThreadPriority(name);
    for (;;) {
        wake_[static_cast<size_t>(index)]->wait();
        if (quit_.load(std::memory_order_acquire))
            return;
        Job& job = jobs_[static_cast<size_t>(index)];
        if (job.graph)
            job.graph->runModel(job.slot, job.buf, job.n);
        done_.post();
    }
}

void NamWorkerPool::run(NamGraph& graph, const int* slots, float* const* bufs,
                        int count, size_t n)
{
    const int dispatched = std::min(count - 1, helpers());

    for (int i = 0; i < dispatched; ++i) {
        jobs_[static_cast<size_t>(i)] = Job{&graph, slots[i + 1], bufs[i + 1], n};
        wake_[static_cast<size_t>(i)]->post();
    }

    // The coordinator takes the first slot, plus anything the pool was too
    // narrow to absorb.
    graph.runModel(slots[0], bufs[0], n);
    for (int i = dispatched + 1; i < count; ++i)
        graph.runModel(slots[i], bufs[i], n);

    // Join before returning. Everything in this stage has now advanced by
    // exactly n samples, so the mix that follows is coherent.
    for (int i = 0; i < dispatched; ++i)
        done_.wait();
}

// ---------------------------------------------------------------------------
// NamEngine
// ---------------------------------------------------------------------------

void NamEngine::init(double rate, size_t maxBlock)
{
    rate_=rate;
    maxBlock_ = std::max<size_t>(maxBlock, 16);
    graph_.init(rate, maxBlock_);
    jobIn_.assign(maxBlock_, 0.0f);
    jobOut_.assign(maxBlock_, 0.0f);
    queued_.assign(2*maxBlock_, 0.0f);
    queuedTime_.assign(2*maxBlock_, 0);
    jobTime_.assign(maxBlock_, 0);
    output_.assign(4*maxBlock_+1, 0.0f);
    outputTime_.assign(output_.size(), UINT64_MAX);
    queueRead_=queueCount_=jobLen_=0;
    time_=0; pending_=false; processed_=false;
    threaded_=requested_=false; transitionGain_=1; fadingIn_=false;
}

void NamEngine::reset()
{
    drain();
    graph_.reset();
    queueRead_=queueCount_=0; time_=0;
    std::fill(outputTime_.begin(), outputTime_.end(), UINT64_MAX);
    pending_=false; processed_=false; transitionGain_=1; fadingIn_=false; gateMeter_=0;
    dropouts_.store(0, std::memory_order_relaxed);
}

void NamEngine::start()
{
    if (coordinator_.joinable())
        return;
    quit_.store(false, std::memory_order_relaxed);
    jobDone_.store(true, std::memory_order_relaxed);
    pool_.start(kNumSlots-1);
    coordinator_ = std::thread([this] { coordinatorLoop(); });
}

void NamEngine::stop()
{
    drain();
    if (coordinator_.joinable()) {
        quit_.store(true, std::memory_order_release);
        jobWake_.post();
        coordinator_.join();
    }
    pool_.stop();
    pending_ = false;
}

// Request only: no waiting or touching worker-owned graph/buffers.
void NamEngine::setThreaded(bool on)
{
    requested_=on;
    if(on==threaded_ && transitionGain_<1) fadingIn_=true;
    if(!processed_ && !pending_) { threaded_=on; }
}

void NamEngine::applyMode()
{
    threaded_=requested_;
    queueRead_=queueCount_=0; time_=0;
    std::fill(outputTime_.begin(),outputTime_.end(),UINT64_MAX);
    fadingIn_=true;
}

// Lifecycle only. Never called from process(), beginBlock() or a mode request.
void NamEngine::drain()
{
    if (!pending_)
        return;
    while (!jobDone_.load(std::memory_order_acquire))
        std::this_thread::yield();
    pending_ = false;
}

void NamEngine::coordinatorLoop()
{
    tryElevateThreadPriority("snam_bg");
    for (;;) {
        jobWake_.wait();
        if (quit_.load(std::memory_order_acquire))
            return;
        runJob();
        jobDone_.store(true, std::memory_order_release);
    }
}

void NamEngine::runJob()
{
    const auto start=std::chrono::steady_clock::now();
    graph_.process(jobIn_.data(), jobOut_.data(), jobLen_,
                   [this](const int* slots, float* const* bufs, int count, size_t n) {
                       if (count == 1)
                           graph_.runModel(slots[0], bufs[0], n);
                       else
                           pool_.run(graph_, slots, bufs, count, n);
                   });
    jobCost_=float(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()*rate_/jobLen_*100);
}

bool NamEngine::beginBlock()
{
    if(pending_) {
        if(!jobDone_.load(std::memory_order_acquire)) return false;
        for(size_t i=0;i<jobLen_;++i) {
            const uint64_t due=jobTime_[i]+maxBlock_;
            // Expired audio must never emerge later at the wrong alignment.
            if(due>=time_ && due-time_<output_.size()) {
                const size_t at=due%output_.size();
                output_[at]=jobOut_[i];outputTime_[at]=due;
            }
        }
        gateMeter_=graph_.gateReduction();
        computePercent_=jobCost_;
        pending_=false;
    }
    if(requested_!=threaded_ && transitionGain_==0) applyMode();
    return true;
}

void NamEngine::dispatch()
{
    if(pending_ || queueCount_==0) return;
    jobLen_=std::min(queueCount_,maxBlock_);
    for(size_t i=0;i<jobLen_;++i) {
        const size_t at=(queueRead_+i)%queued_.size();
        jobIn_[i]=queued_[at];jobTime_[i]=queuedTime_[at];
    }
    queueRead_=(queueRead_+jobLen_)%queued_.size();queueCount_-=jobLen_;
    jobDone_.store(false,std::memory_order_release);
    pending_=true;jobWake_.post();
}

void NamEngine::process(const float* in,float* out,size_t n)
{
    if(n==0) return;
    processed_=true;
    bool dropout=false;
    if(!threaded_) {
        const auto start=std::chrono::steady_clock::now();
        // Inline can safely subdivide a host block larger than our work buffers.
        for(size_t off=0;off<n;off+=maxBlock_)
            graph_.processInline(in+off,out+off,std::min(n-off,maxBlock_));
        gateMeter_=graph_.gateReduction();
        computePercent_=float(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()*rate_/n*100);
    } else {
        // Input and output timestamps share a single monotonic sample clock.
        // Enqueue before emitting, so actual in-place LV2 buffers are safe.
        for(size_t i=0;i<n;++i) {
            if(queueCount_<queued_.size()) {
                const size_t at=(queueRead_+queueCount_)%queued_.size();
                queued_[at]=in[i];queuedTime_[at]=time_+i;++queueCount_;
            } else dropout=true;
        }
        // Oversized calls violate maxBlockLength. Emit/count missing deadlines,
        // rather than truncating the host's buffer or silently dropping tails.
        for(size_t i=0;i<n;++i) {
            const uint64_t now=time_+i;const size_t at=now%output_.size();
            if(outputTime_[at]==now) {out[i]=output_[at];outputTime_[at]=UINT64_MAX;}
            else {out[i]=0; if(now>=maxBlock_)dropout=true;}
        }
        if(!(requested_!=threaded_ && transitionGain_==0)) dispatch();
    }
    for(size_t i=0;i<n;++i) {
        if(requested_!=threaded_) transitionGain_=std::max(0.0f,transitionGain_-1.0f/64);
        else if(fadingIn_ && (!threaded_ || time_+i>=maxBlock_)) {
            transitionGain_=std::min(1.0f,transitionGain_+1.0f/64);
            if(transitionGain_==1)fadingIn_=false;
        }
        out[i]*=transitionGain_;
    }
    time_+=n;
    if(dropout)dropouts_.fetch_add(1,std::memory_order_relaxed);
}

} // namespace supr
