#ifndef LINRAD_LDPC_POOL_H
#define LINRAD_LDPC_POOL_H
#include <atomic>
#include <cstdint>
#include <vector>

struct LdpcFrame {
    int modcod, short_frame;
    std::vector<int8_t> data;
    std::atomic<bool> done{false};
    bool failed = false;
};
struct LdpcPool;
LdpcPool *ldpc_pool_create(const char *command, unsigned workers, unsigned batch,
                           unsigned batch_wait_ms = 2);
void ldpc_pool_submit(LdpcPool *, LdpcFrame *);
// Submit an already collected batch atomically, without another collection delay.
void ldpc_pool_submit_batch(LdpcPool *, const std::vector<LdpcFrame *>&);
void ldpc_pool_destroy(LdpcPool *);
#endif
