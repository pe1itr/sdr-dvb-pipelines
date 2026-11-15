#ifndef DVB_BONDING_H
#define DVB_BONDING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOND_TS_SIZE 188u
#define BOND_TS_PER_SYMBOL 7u
#define BOND_PAYLOAD_SIZE (BOND_TS_SIZE * BOND_TS_PER_SYMBOL)
#define BOND_HEADER_SIZE 28u
#define BOND_FRAME_SIZE (BOND_HEADER_SIZE + BOND_PAYLOAD_SIZE)
#define BOND_MAX_GROUP 16u
#define BOND_MAX_WINDOW 64u

typedef struct bond_splitter bond_splitter;
typedef struct bond_merger bond_merger;

/* Called synchronously. Copy the frame before returning if it must be queued.
 * The callback must accept every frame; flow control belongs to the adapter.
 * Instances are single-threaded and callbacks must not re-enter them. */
typedef void (*bond_emit_fn)(void *context, unsigned lane,
                             const uint8_t frame[BOND_FRAME_SIZE]);

/* group_size counts source symbols, each containing seven TS packets. */
bond_splitter *bond_splitter_create(uint64_t session, unsigned group_size,
                                    bond_emit_fn emit, void *context);
void bond_splitter_destroy(bond_splitter *splitter);
/* Input must be aligned, complete 188-byte TS packets. Returns -1 on error. */
int bond_splitter_push(bond_splitter *splitter,
                       const uint8_t packet[BOND_TS_SIZE]);
/* Emits a partial final group. No further push is allowed after finish. */
int bond_splitter_finish(bond_splitter *splitter);

typedef struct {
    uint64_t accepted, rejected, duplicates, late, outside_window;
    uint64_t recovered_symbols, missing, emitted;
} bond_stats;

/* Both endpoints must agree on session and group size out of band.
 * window_groups bounds memory and accepted look-ahead. */
bond_merger *bond_merger_create(uint64_t session, unsigned group_size,
                               unsigned window_groups);
/* Join a live stream at an aligned group base, rather than source symbol zero. */
bond_merger *bond_merger_create_at(uint64_t session, unsigned group_size,
                                  unsigned window_groups, uint64_t group_base);
int bond_merger_reset(bond_merger *merger, uint64_t session, uint64_t group_base);
typedef struct {
    uint64_t session, group_base;
    unsigned source_count, symbol_index;
} bond_frame_info;
/* Validate framing/CRC before trusting the sequence used for a live join. */
int bond_frame_inspect(const uint8_t *frame, size_t length, bond_frame_info *info);
void bond_merger_destroy(bond_merger *merger);
/* Returns 0 for accepted/duplicate/late frames; -1 for malformed, wrong-session
 * or out-of-window frames. Integrity is CRC32, not authentication. */
int bond_merger_receive(bond_merger *merger, const uint8_t *frame, size_t length);
/* Call once at each source-packet playout deadline, starting at sequence zero.
 * Valid calls always advance: 1 = original/recovered packet; 0 = replacement
 * null packet. Returns -1 for invalid arguments or exhausted sequence space.
 * Late data can never alter already emitted packets. Caller supplies timing,
 * total packet count/end-of-stream and stream restart policy out of band. */
int bond_merger_pop(bond_merger *merger, uint8_t packet[BOND_TS_SIZE]);
const bond_stats *bond_merger_stats(const bond_merger *merger);

#ifdef __cplusplus
}
#endif

#endif
