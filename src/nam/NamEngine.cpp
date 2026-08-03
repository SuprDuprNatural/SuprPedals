// NamEngine.cpp — see NamEngine.h.
//
// MIT license, (c) 2026 SuprPedals contributors.

#include "NamEngine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

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
    maxBlock_ = std::max<size_t>(maxBlock, 16);
    graph_.init(rate, maxBlock_);
    jobIn_.assign(maxBlock_, 0.0f);
    jobOut_.assign(maxBlock_, 0.0f);
    carry_.assign(maxBlock_, 0.0f);
    jobLen_   = 0;
    carryLen_ = 0;
    pending_  = false;
}

void NamEngine::reset()
{
    drain();
    graph_.reset();
    std::fill(carry_.begin(), carry_.end(), 0.0f);
    carryLen_ = 0;
    pending_  = false;
    dropouts_.store(0, std::memory_order_relaxed);
}

void NamEngine::start()
{
    if (coordinator_.joinable())
        return;
    quit_.store(false, std::memory_order_relaxed);
    jobDone_.store(true, std::memory_order_relaxed);
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

// Switching modes changes the plugin's latency, so it will click either way.
// What must not happen is a half-drained pipeline surviving the switch: that
// would leave one block of stale audio to be emitted after the delay had
// already gone, which is the one thing worse than a click.
void NamEngine::setThreaded(bool on)
{
    if (on == threaded_)
        return;
    drain();
    threaded_ = on;
    carryLen_ = 0;
    pending_  = false;
}

// Waits out an in-flight job. Only ever called from a user-initiated
// transition (threading toggled, plugin deactivated), never per block.
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
    // The pool is sized to the routing rather than to a fixed maximum, so a
    // Single or a pure-series routing costs no helper threads at all.
    const int width = graph_.plan().width();
    if (width != currentWidth_) {
        currentWidth_ = width;
        pool_.start(width - 1);
    }

    graph_.process(jobIn_.data(), jobOut_.data(), jobLen_,
                   [this](const int* slots, float* const* bufs, int count, size_t n) {
                       if (count == 1)
                           graph_.runModel(slots[0], bufs[0], n);
                       else
                           pool_.run(graph_, slots, bufs, count, n);
                   });
}

bool NamEngine::beginBlock()
{
    if (!threaded_ || !pending_)
        return true;
    if (!jobDone_.load(std::memory_order_acquire))
        return false; // worker still busy — leave the graph alone this block

    std::copy(jobOut_.begin(), jobOut_.begin() + static_cast<long>(jobLen_), carry_.begin());
    carryLen_ = jobLen_;
    pending_  = false;
    return true;
}

void NamEngine::process(const float* in, float* out, size_t n)
{
    n = std::min(n, maxBlock_);

    if (!threaded_) {
        graph_.processInline(in, out, n);
        return;
    }

    // beginBlock() harvests a finished job. Still pending here means the
    // workers did not make the deadline: emit silence and skip this block's
    // job rather than letting a second one queue up behind the first.
    if (pending_) {
        std::fill(out, out + n, 0.0f);
        dropouts_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Emit the previous block's finished output. carryLen_ of 0 is the one
    // priming block after threading is switched on.
    const size_t emit = std::min(carryLen_, n);
    std::copy(carry_.begin(), carry_.begin() + static_cast<long>(emit), out);
    std::fill(out + emit, out + n, 0.0f);
    carryLen_ = 0;

    // Hand this block over whole.
    std::copy(in, in + n, jobIn_.begin());
    jobLen_ = n;
    jobDone_.store(false, std::memory_order_release);
    pending_ = true;
    jobWake_.post();
}

} // namespace supr
