#ifndef DSMCC_MESSAGES_H
#define DSMCC_MESSAGES_H
#include <glib.h>
#include <json-c/json.h>

/* Owned by the receiver, shared by the TS relay and the HTTP main context. */
typedef struct {
    GMutex mutex;
    GQueue entries;
    GHashTable *ids;
    gint64 latest;
} DsmccMessages;

void dsmcc_messages_section(DsmccMessages *messages, guint pid, const guint8 *bytes, guint size);
json_object *dsmcc_messages_status(DsmccMessages *messages);
json_object *dsmcc_messages_read(DsmccMessages *messages, gint64 after);
void dsmcc_messages_expire(DsmccMessages *messages);
void dsmcc_messages_reset(DsmccMessages *messages);
void dsmcc_messages_clear(DsmccMessages *messages);
#endif
