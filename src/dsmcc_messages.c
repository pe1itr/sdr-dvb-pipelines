#include "dsmcc_messages.h"
#include <string.h>

/* The Mosaic carousel has one uncompressed module in DDB block zero.
 * Parse its BIOP gateway binding and file object, using counted fields
 * throughout. Other carousel profiles are outside this message protocol. */
typedef struct { const guint8 *p; guint n; } Slice;

static Slice take(Slice *s, guint n) {
    if (!s->p || n>s->n) {
        *s=(Slice){0};
        return *s;
    }
    Slice part={s->p,n};
    s->p+=n;
    s->n-=n;
    return part;
}

static guint32 number(Slice *s, guint width) {
    Slice v=take(s,width);
    guint32 result=0;
    if (v.p) for (guint i=0;i<width;i++) result=(result<<8)|v.p[i];
    return result;
}

static Slice counted(Slice *s, guint width) {
    guint32 length=number(s,width);
    return take(s,length);
}

static gboolean equals(Slice s, const void *data, guint n) {
    return s.p && s.n==n && !memcmp(s.p,data,n);
}

static Slice object_body(Slice *module, const char *kind, Slice *key) {
    if (!equals(take(module,8),"BIOP\1\0\0\0",8)) return (Slice){0};
    Slice object=counted(module,4);
    *key=counted(&object,1);
    if (!equals(counted(&object,4),kind,4)) return (Slice){0};
    counted(&object,2); /* object info */
    if (number(&object,1)!=0) return (Slice){0}; /* service contexts */
    Slice body=counted(&object,4);
    return object.n ? (Slice){0} : body;
}

static Slice station_key(Slice root, guint32 carousel, guint module_id) {
    if (number(&root,2)!=1 || number(&root,1)!=1 ||
        !equals(counted(&root,1),"station.json",13) ||
        !equals(counted(&root,1),"fil",4) || number(&root,1)!=1 ||
        !equals(counted(&root,4),"fil",4) || number(&root,4)!=1 ||
        number(&root,4)!=0x49534f06) return (Slice){0};
    Slice profile=counted(&root,4);
    counted(&root,2); /* binding object info */
    if (!root.p || root.n || number(&profile,1)!=0 || number(&profile,1)<1 ||
        number(&profile,4)!=0x49534f50) return (Slice){0};
    Slice location=counted(&profile,1);
    if (number(&location,4)!=carousel || number(&location,2)!=module_id ||
        number(&location,2)!=0x0100) return (Slice){0};
    return counted(&location,1);
}

static const char *string_field(json_object *obj, const char *name) {
    json_object *field=json_object_object_get(obj,name);
    if (!json_object_is_type(field,json_type_string)) return NULL;
    const char *s=json_object_get_string(field);
    return *s && strlen(s)==(size_t)json_object_get_string_len(field) ? s : NULL;
}

static gboolean valid_message(json_object *msg) {
    if (!json_object_is_type(msg,json_type_object)) return FALSE;
    const char *id=string_field(msg,"id"), *type=string_field(msg,"type");
    if (!id || !g_uuid_string_is_valid(id) || !type ||
        !string_field(msg,"from") || !string_field(msg,"to")) return FALSE;
    json_object *path=NULL;
    if (json_object_object_get_ex(msg,"path",&path)) {
        if (!json_object_is_type(path,json_type_array)) return FALSE;
        for (size_t i=0;i<json_object_array_length(path);i++)
            if (!json_object_is_type(json_object_array_get_idx(path,i),json_type_string)) return FALSE;
    }
    if (!strcmp(type,"chat")) return string_field(msg,"text")!=NULL;
    const char *reply=string_field(msg,"reply_to");
    return !strcmp(type,"response") && reply && g_uuid_string_is_valid(reply);
}

/* Cursor values fit in a JavaScript safe integer with current Unix times.
 * A new process starts a fresh cursor range, so stale clients see a gap. */
static gint64 latest(DsmccMessages *m) {
    if (!m->latest) m->latest=g_get_real_time();
    return m->latest;
}

typedef struct {
    gint64 expires_at;
    char *id;
    json_object *entry;
} ReceivedMessage;

static void remove_oldest(DsmccMessages *m) {
    ReceivedMessage *old=g_queue_pop_head(&m->entries);
    g_hash_table_remove(m->ids,old->id);
    g_free(old->id);
    json_object_put(old->entry);
    g_free(old);
}

/* Called under the mutex. The queue is ordered by first reception. */
static void expire_locked(DsmccMessages *m, gint64 now) {
    ReceivedMessage *first;
    while ((first=g_queue_peek_head(&m->entries)) && first->expires_at<=now)
        remove_oldest(m);
}

void dsmcc_messages_expire(DsmccMessages *m) {
    g_mutex_lock(&m->mutex);
    expire_locked(m,g_get_monotonic_time());
    g_mutex_unlock(&m->mutex);
}

static void record(DsmccMessages *m, guint pid, json_object *msg) {
    char *id=g_ascii_strdown(string_field(msg,"id"),-1);
    g_mutex_lock(&m->mutex);
    gint64 now=g_get_monotonic_time();
    expire_locked(m,now);
    if (!m->ids) m->ids=g_hash_table_new(g_str_hash,g_str_equal);
    if (g_hash_table_contains(m->ids,id)) {
        g_mutex_unlock(&m->mutex);
        g_free(id);
        return;
    }
    ReceivedMessage *received=g_new(ReceivedMessage,1);
    received->expires_at=now+3600LL*G_USEC_PER_SEC;
    received->id=id;
    json_object *entry=received->entry=json_object_new_object();
    latest(m);
    json_object_object_add(entry,"cursor",json_object_new_int64(++m->latest));
    json_object_object_add(entry,"received_unix_ms",json_object_new_int64(g_get_real_time()/1000));
    json_object_object_add(entry,"pid",json_object_new_int(pid));
    json_object_object_add(entry,"message",json_object_get(msg));
    g_hash_table_add(m->ids,id);
    g_queue_push_tail(&m->entries,received);
    g_mutex_unlock(&m->mutex);
}

void dsmcc_messages_section(DsmccMessages *m, guint pid, const guint8 *bytes, guint size) {
    /* Section assembler has verified the CRC, current_next and section length. */
    if (size<30 || bytes[0]!=0x3c || bytes[6] || bytes[7]) return;
    Slice header={bytes+8,size-12};
    if (number(&header,4)!=0x11031003) return;
    guint32 carousel=number(&header,4);
    number(&header,1);
    guint adaptation=number(&header,1);
    Slice block=counted(&header,2);
    if (header.n) return;
    take(&block,adaptation);
    guint module_id=number(&block,2);
    number(&block,2); /* version and reserved */
    if (number(&block,2)!=0) return;
    Slice key;
    Slice root=object_body(&block,"srg",&key);
    Slice target=station_key(root,carousel,module_id);
    if (!target.p || !target.n) return;
    Slice body=object_body(&block,"fil",&key);
    if (block.n || !equals(key,target.p,target.n)) return;
    Slice file=counted(&body,4);
    if (!file.p || !file.n || body.n || !g_utf8_validate((const char *)file.p,file.n,NULL)) return;
    json_tokener *tok=json_tokener_new();
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
    json_object *msg=json_tokener_parse_ex(tok,(const char *)file.p,file.n);
    if (json_tokener_get_error(tok)==json_tokener_success &&
        json_tokener_get_parse_end(tok)==file.n && valid_message(msg)) record(m,pid,msg);
    json_object_put(msg);
    json_tokener_free(tok);
}

json_object *dsmcc_messages_status(DsmccMessages *m) {
    json_object *obj=json_object_new_object();
    g_mutex_lock(&m->mutex);
    json_object_object_add(obj,"latest_cursor",json_object_new_int64(latest(m)));
    g_mutex_unlock(&m->mutex);
    json_object_object_add(obj,"href",json_object_new_string("/dsmcc/messages"));
    return obj;
}

json_object *dsmcc_messages_read(DsmccMessages *m, gint64 after) {
    json_object *obj=json_object_new_object(), *items=json_object_new_array();
    g_mutex_lock(&m->mutex);
    expire_locked(m,g_get_monotonic_time());
    gint64 last=latest(m);
    gboolean gap=after>=0 && (after>last || after<last-m->entries.length);
    for (GList *l=m->entries.head;l;l=l->next) {
        ReceivedMessage *received=l->data;
        if (!gap && json_object_get_int64(json_object_object_get(received->entry,"cursor"))<=after) continue;
        /* json-c refcounts/serialization caches are not thread-safe. */
        json_object *copy=NULL;
        json_object_deep_copy(received->entry,&copy,NULL);
        json_object_array_add(items,copy);
    }
    g_mutex_unlock(&m->mutex);
    json_object_object_add(obj,"messages",items);
    json_object_object_add(obj,"latest_cursor",json_object_new_int64(last));
    json_object_object_add(obj,"gap",json_object_new_boolean(gap));
    return obj;
}

void dsmcc_messages_reset(DsmccMessages *m) {
    g_mutex_lock(&m->mutex);
    while (!g_queue_is_empty(&m->entries)) remove_oldest(m);
    g_clear_pointer(&m->ids,g_hash_table_unref);
    /* Invalidate cursors from the stopped receiver, including its latest. */
    m->latest=MAX(m->latest+1,g_get_real_time());
    g_mutex_unlock(&m->mutex);
}

void dsmcc_messages_clear(DsmccMessages *m) {
    dsmcc_messages_reset(m);
    g_mutex_clear(&m->mutex);
}
