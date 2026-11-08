#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
#include "dect2/common/stage_prof.h"

// DVB-T2 keeps FEC work buffers in thread_local objects. Verify that the
// packaged compiler runtime runs their destructors when a worker exits.
static std::atomic<int> destroyed{0};
struct Work {
    std::vector<int> data = std::vector<int>(4096, 42);
    ~Work() { assert(data.back() == 42); ++destroyed; }
};
static void worker()
{
    static thread_local Work work;
    assert(work.data.front() == 42);
}
int main()
{
    // Live profiling must measure work, rather than time blocked on input.
    const auto idle0 = dect2::prof::threadCpuNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto idle1 = dect2::prof::threadCpuNs();
    assert(idle1 >= idle0 && idle1 - idle0 < 100000000);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < end) std::atomic_signal_fence(std::memory_order_seq_cst);
    assert(dect2::prof::threadCpuNs() > idle1);
    for (int i = 0; i < 8; ++i) {
        std::thread thread(worker);
        thread.join();
        assert(destroyed == i + 1);
    }
    return 0;
}
