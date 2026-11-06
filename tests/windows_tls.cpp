#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

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
    for (int i = 0; i < 8; ++i) {
        std::thread thread(worker);
        thread.join();
        assert(destroyed == i + 1);
    }
    return 0;
}
