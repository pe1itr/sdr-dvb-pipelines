#include "ldpc_pool.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#ifndef _WIN32
#include <spawn.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
struct LdpcPool {
    std::string command;
    unsigned batch;
    bool stopping = false;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<LdpcFrame *> queue;
    std::vector<std::thread> threads;
};

static bool transfer(int fd, void *data, size_t size, bool writing) {
    char *p = static_cast<char *>(data);
    while (size) {
        ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}

/* Each thread owns one persistent child. No DSP/GUI locks are held during I/O.
 * Helpers inherit the decoder process group; Stop terminates the whole group. */
static void worker(LdpcPool *pool) {
#ifndef _WIN32
    sigset_t signals; sigemptyset(&signals); sigaddset(&signals, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
#endif
    int tx = -1, rx = -1, mc = -1, sf = -1;
#ifdef _WIN32
    GPid child = nullptr;
#else
    pid_t child = -1;
#endif
    auto close_child = [&]() {
        if (tx >= 0) close(tx);
        if (rx >= 0) close(rx);
        tx = rx = -1;
#ifdef _WIN32
        if (child) {
            lw_stop_process(child);
            lw_waitpid(child, nullptr, 0);
            g_spawn_close_pid(child);
        }
        child = nullptr;
#else
        if (child > 0) {
            kill(child, SIGTERM);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        }
        child = -1;
#endif
    };
    for (;;) {
        std::vector<LdpcFrame *> frames;
        {
            std::unique_lock<std::mutex> lock(pool->mutex);
            pool->wake.wait(lock, [&] { return pool->stopping || !pool->queue.empty(); });
            if (pool->queue.empty()) break;
            // At most 2 ms batching delay, even for low symbol rates and EOF.
            if (pool->batch > 1 && pool->queue.size() < pool->batch && !pool->stopping)
                pool->wake.wait_for(lock, std::chrono::milliseconds(2), [&] {
                    return pool->stopping || pool->queue.size() >= pool->batch;
                });
            if (pool->queue.empty()) continue; // Another worker may have taken it while we waited.
            LdpcFrame *first = pool->queue.front();
            while (!pool->queue.empty() && frames.size() < pool->batch) {
                auto f = pool->queue.front();
                if (f->modcod != first->modcod || f->short_frame != first->short_frame) break;
                frames.push_back(f); pool->queue.pop_front();
            }
        }
        auto first = frames.front();
        bool ok = true;
        if (
#ifdef _WIN32
            !child
#else
            child < 0
#endif
            || mc != first->modcod || sf != first->short_frame) {
            close_child();
            int input[2], output[2];
            ok = pipe2(input, O_CLOEXEC) == 0;
            if (ok && pipe2(output, O_CLOEXEC) < 0) {
                close(input[0]); close(input[1]); ok = false;
            }
            if (ok) {
                std::string modcod = std::to_string(first->modcod);
                const char *args[] = {pool->command.c_str(), "--framed", "--modcod", modcod.c_str(),
                    first->short_frame ? "--shortframes" : nullptr, nullptr};
#ifdef _WIN32
                GError *spawn_error = nullptr;
                int error = !g_spawn_async_with_fds(nullptr, const_cast<char **>(args), nullptr,
                    G_SPAWN_DO_NOT_REAP_CHILD, nullptr, nullptr, &child,
                    input[0], output[1], 2, &spawn_error);
                if (spawn_error) {
                    fprintf(stderr, "LDPC: %s\n", spawn_error->message);
                    g_error_free(spawn_error);
                }
#else
                posix_spawn_file_actions_t actions;
                posix_spawn_file_actions_init(&actions);
                posix_spawn_file_actions_adddup2(&actions, input[0], 0);
                posix_spawn_file_actions_adddup2(&actions, output[1], 1);
                int error = posix_spawn(&child, pool->command.c_str(), &actions, nullptr,
                                       const_cast<char **>(args), environ);
                posix_spawn_file_actions_destroy(&actions);
#endif
                close(input[0]); close(output[1]);
                tx = input[1]; rx = output[0]; ok = error == 0;
                if (!ok) fprintf(stderr, "LDPC helper could not start (%d): %s\n", error, pool->command.c_str());
                mc = first->modcod; sf = first->short_frame;
            }
        }
        uint32_t count = frames.size();
        if (ok) ok = transfer(tx, &count, sizeof(count), true);
        for (auto f : frames) if (ok) ok = transfer(tx, f->data.data(), f->data.size(), true);
        for (auto f : frames) if (ok) ok = transfer(rx, f->data.data(), f->data.size(), false);
        if (!ok) close_child();
        for (auto f : frames) { f->failed = !ok; f->done.store(true, std::memory_order_release); }
    }
    close_child();
}

LdpcPool *ldpc_pool_create(const char *command, unsigned workers, unsigned batch) {
    auto p = new LdpcPool;
    p->command = command; p->batch = batch;
    for (unsigned i = 0; i < workers; ++i) p->threads.emplace_back(worker, p);
    return p;
}
void ldpc_pool_submit(LdpcPool *p, LdpcFrame *frame) {
    { std::lock_guard<std::mutex> lock(p->mutex); p->queue.push_back(frame); }
    p->wake.notify_one();
}
void ldpc_pool_destroy(LdpcPool *p) {
    if (!p) return;
    { std::lock_guard<std::mutex> lock(p->mutex); p->stopping = true; }
    p->wake.notify_all();
    for (auto &t : p->threads) t.join();
    delete p;
}
