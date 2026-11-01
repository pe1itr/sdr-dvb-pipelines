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
LdpcPool *ldpc_pool_create(const char *command, unsigned workers, unsigned batch);
void ldpc_pool_submit(LdpcPool *, LdpcFrame *);
void ldpc_pool_destroy(LdpcPool *);
#endif
