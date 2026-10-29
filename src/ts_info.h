#ifndef TS_INFO_H
#define TS_INFO_H
#include <glib.h>

#define TS_INFO_SERVICES 32
#define TS_INFO_QUEUE_LIMIT (1024u * 1024)
typedef struct { guint16 id, pmt_pid; char name[128]; } TsInfoService;
typedef struct {
    gboolean valid;
    guint16 id;
    gint64 start; /* UTC Unix seconds, -1 if unspecified */
    int duration; /* seconds, -1 if unspecified */
    char language[4], title[512], text[8192];
} TsInfoEvent;
typedef struct {
    TsInfoService services[TS_INFO_SERVICES];
    unsigned count;
    guint16 selected;
    gboolean pmt_seen, teletext_announced, teletext_decoder;
    TsInfoEvent events[2];
    gint64 eit_received, teletext_received; /* wall-clock microseconds */
    GBytes *page; /* immutable native Cairo RGB24 pixels */
    int width, height;
} TsInfoSnapshot;
void ts_info_snapshot_clear(TsInfoSnapshot *s);

/* The synchronous parser is worker-owned; also usable with recorded TS in tests. */
typedef struct TsInfoParser TsInfoParser;
TsInfoParser *ts_info_parser_new(void);
void ts_info_parser_free(TsInfoParser *p);
void ts_info_parser_feed(TsInfoParser *p, const guint8 *data, gsize size);
void ts_info_parser_gap(TsInfoParser *p);
void ts_info_parser_select(TsInfoParser *p, guint16 service);
void ts_info_parser_snapshot(TsInfoParser *p, TsInfoSnapshot *out);

typedef struct TsInfoBackend TsInfoBackend;
TsInfoBackend *ts_info_backend_new(void);
void ts_info_backend_feed(TsInfoBackend *b, const guint8 *data, gsize size);
void ts_info_backend_reset(TsInfoBackend *b);
void ts_info_backend_select(TsInfoBackend *b, guint16 service);
void ts_info_backend_snapshot(TsInfoBackend *b, TsInfoSnapshot *out);
void ts_info_backend_finish(TsInfoBackend *b);
void ts_info_backend_stop(TsInfoBackend *b);
gboolean ts_info_backend_done(TsInfoBackend *b);
void ts_info_backend_free(TsInfoBackend *b);
#endif
