#ifndef LINRAD_SCATTER_BACKEND_H
#define LINRAD_SCATTER_BACKEND_H
#include <glib.h>
#include "recovery.h"
#include "ts_info.h"

#define SCATTER_QUEUE_LIMIT (4u * 1024 * 1024)
typedef struct {
    TsInfoBackend *information;
    GMutex mutex;
    GCond wake;
    GQueue queue;
    size_t queued;
    gboolean eof, gap, enabled;
    guint64 generation, worker_generation;
    gint stopping, relay_done, worker_done, relay_failed, recovery_failed;
    GThread *relay, *worker;
    int input, output;
    SfRecoveryStatus status;
    GBytes *image;
    int width, height, stride;
    guint64 dropped;
} ScatterBackend;

/* Duplicates the descriptors; caller retains ownership. Only the recovery
 * branch can drop data. The output receives every input byte unchanged. */
ScatterBackend *scatter_backend_start(int input, int output, gboolean enabled, GError **error);
/* Switch only recovery; the original TS relay always keeps running. */
void scatter_backend_set_enabled(ScatterBackend *s, gboolean enabled);
void scatter_backend_stop(ScatterBackend *s);
gboolean scatter_backend_done(ScatterBackend *s);
/* Call after done (or outside the GTK event loop). */
void scatter_backend_destroy(ScatterBackend *s);
#endif
