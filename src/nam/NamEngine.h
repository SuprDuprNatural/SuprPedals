// NamEngine.h — runs NamGraph either inline on the audio thread or deferred
// onto worker threads, without the two sounding different.
//
// WHY THREADING EXISTS. A Pi 4 has four cores and one audio thread. Measured
// on one at 1.8 GHz, a standard-architecture Tone3000 model costs about 46%
// of a core, so a single model inline is comfortable and two are not — that is
// already over 90% of the one core the audio thread gets, before the rest of
// the pedalboard. Moving inference off that thread is the only way to reach
// the other three cores, and it is the difference between "three models" being
// a feature and being a promise the plugin cannot keep.
//
// WHY IT IS THE BIGGEST RISK TO GET WRONG. The instant one path in a parallel
// stage is delayed by a buffer and another is not, the sum combs catastrophically
// — a 64-sample offset at 48 kHz puts the first null at about 375 Hz, right
// through the middle of a guitar. So the deferral here is all-or-nothing:
//
//     THE WHOLE WET CHAIN FOR A BLOCK IS DEFERRED, OR NONE OF IT IS.
//
// The audio thread hands over block N in one piece and collects block N-1's
// finished output. Everything — input gain, gate, filters, every model, the
// mix, output gain — happens inside that one deferral. Every path therefore
// carries exactly the same one-block delay, for every routing, whether a stage
// runs on one thread or three. There is no arrangement of models or topology
// that can put two parallel paths at different delays, because no path has a
// delay of its own to get wrong.
//
// Within the deferred block, a parallel stage still fans out across helper
// threads — that is where the extra cores actually get used — but the
// coordinator joins every helper before the stage's outputs are mixed, so the
// fan-out is invisible in the result. Series stages stay sequential, since
// their dependency is real; threading them would mean pipelining, and
// pipelining would mean per-stage latency, and per-stage latency is exactly
// what must not happen.
//
// Latency is reported to the host as one maximum block, on a port carrying
// lv2:designation lv2:latency. Worth knowing: PiPedal never reads that port —
// it has no delay compensation at all — so inside a pedalboard split the other
// branch will not be pulled back to match. The port is there for hosts that do
// compensate, and because reporting latency honestly is cheap; it is not a
// promise that anything acts on it.
//
// MUTATING THE GRAPH SAFELY. The coordinator reads graph state while it works,
// so the audio thread may only touch parameters or swap models while the
// worker is idle. beginBlock() reports when that is; on the rare block where
// the worker is still busy (i.e. we are already overloaded) changes are simply
// held over to the next one, which is inaudible and needs no locks anywhere
// near the audio thread.
//
// MIT license, (c) 2026 SuprPedals contributors.

#pragma once

#include "../NamDsp.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#ifdef __linux__
#include <semaphore.h>
#else
#include <condition_variable>
#include <mutex>
#endif

namespace supr {

// A counting semaphore.
//
// On Linux — the only platform this plugin actually runs on — this is a POSIX
// sem_t, because sem_post does not take a lock in the uncontended case and is
// therefore safe to call from the audio thread. A condition variable would
// not be: notify_one has to acquire the waiter's mutex, and an audio thread
// that can block on a mutex is an audio thread that can xrun.
//
// Elsewhere (macOS, which does not implement unnamed POSIX semaphores) it
// falls back to a condition variable. That path exists so the offline tests
// build and run on a development machine; it is not real-time safe and never
// executes on a Pi.
class Semaphore {
public:
    Semaphore(const Semaphore&)            = delete;
    Semaphore& operator=(const Semaphore&) = delete;

#ifdef __linux__
    Semaphore() { sem_init(&sem_, 0, 0); }
    ~Semaphore() { sem_destroy(&sem_); }

    void post() { sem_post(&sem_); }
    void wait()
    {
        while (sem_wait(&sem_) != 0)
            ; // EINTR
    }

private:
    sem_t sem_;
#else
    Semaphore() = default;

    void post()
    {
        {
            std::lock_guard lock{mutex_};
            ++count_;
        }
        cv_.notify_one();
    }
    void wait()
    {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    unsigned                count_ = 0;
#endif
};

// Helper threads for the slots of a parallel stage. The coordinator takes one
// slot itself and hands the rest out, so a three-wide stage occupies three
// cores and a one-wide stage occupies one.
class NamWorkerPool {
public:
    ~NamWorkerPool() { stop(); }

    void start(int helpers);
    void stop();
    int  helpers() const { return static_cast<int>(threads_.size()); }

    // Runs count slots concurrently and returns only when all of them have
    // advanced exactly n samples. That join is the guarantee the parallel sum
    // downstream depends on.
    void run(NamGraph& graph, const int* slots, float* const* bufs, int count, size_t n);

private:
    struct Job {
        NamGraph* graph = nullptr;
        int       slot  = 0;
        float*    buf   = nullptr;
        size_t    n     = 0;
    };

    void helperLoop(int index);

    std::vector<std::thread>                 threads_;
    std::vector<std::unique_ptr<Semaphore>>  wake_;
    std::vector<Job>                         jobs_;
    Semaphore                                done_;
    std::atomic<bool>                        quit_{false};
};

// Names the calling thread and puts it on the real-time scheduler just below
// the audio thread. Fails quietly without the privilege — the pool still
// works, it is just at the mercy of the ordinary scheduler. See the .cpp for
// where the numbers come from.
void tryElevateThreadPriority(const char* name);

class NamEngine {
public:
    ~NamEngine() { stop(); }

    void init(double rate, size_t maxBlock);
    void reset();

    // Bracket the plugin's active life. The coordinator thread exists for the
    // whole of it regardless of the Threaded setting — it costs nothing parked
    // on a semaphore, and it keeps thread creation out of Run().
    void start();
    void stop();

    NamGraph&       graph() { return graph_; }
    const NamGraph& graph() const { return graph_; }

    // Threading is a whole-plugin decision, never a per-path one.
    void setThreaded(bool on);
    bool threaded() const { return threaded_; }

    // One maximum block when threaded, zero when inline.
    size_t latencySamples() const { return threaded_ ? maxBlock_ : 0; }

    // Call once at the top of each Run(), before touching the graph. Returns
    // false if the worker is still busy, in which case the graph must be left
    // alone until the next block.
    bool beginBlock();

    // Call once per Run(), after any graph changes.
    void process(const float* in, float* out, size_t n);

    // Blocks worth of audio dropped because the workers could not keep up.
    // Surfaced in the UI so an overload looks like an overload rather than a
    // mystery crackle.
    uint64_t dropouts() const { return dropouts_.load(std::memory_order_relaxed); }
    void clearDropouts() { dropouts_.store(0, std::memory_order_relaxed); }

private:
    void drain();
    void coordinatorLoop();
    void runJob();

    NamGraph graph_;
    size_t   maxBlock_ = 512;
    bool     threaded_ = false;

    std::thread       coordinator_;
    NamWorkerPool     pool_;
    Semaphore         jobWake_;
    std::atomic<bool> jobDone_{true};
    std::atomic<bool> quit_{false};
    std::atomic<uint64_t> dropouts_{0};

    // Only one job is ever in flight, so these need no double buffering: the
    // audio thread touches them exactly when the worker does not.
    std::vector<float> jobIn_;
    std::vector<float> jobOut_;
    std::vector<float> carry_;
    size_t             jobLen_   = 0;
    size_t             carryLen_ = 0;
    bool               pending_  = false;

    // How wide the pool needs to be for the current routing.
    int currentWidth_ = 1;
};

} // namespace supr
