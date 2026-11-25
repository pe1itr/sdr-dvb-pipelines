#include "dsmcc_messages.h"
#include <string.h>

/* The receiver uses monotonic time for retention, independent of UTC. */
static gint64 now;
gint64 __wrap_g_get_monotonic_time(void) { return now; }

static void check_page(DsmccMessages *messages, gint64 after, guint count, gboolean gap) {
    json_object *page=dsmcc_messages_read(messages,after);
    g_assert_cmpuint(json_object_array_length(json_object_object_get(page,"messages")),==,count);
    g_assert_cmpint(json_object_get_boolean(json_object_object_get(page,"gap")),==,gap);
    json_object_put(page);
}

int main(void) {
    char *hex=NULL;
    g_assert_true(g_file_get_contents("tests/fixtures/mosaic-station-ddb.hex",&hex,NULL,NULL));
    GByteArray *section=g_byte_array_new();
    for (char *p=g_strstrip(hex);*p;p++) {
        if (g_ascii_isspace(*p)) continue;
        guint8 byte=(g_ascii_xdigit_value(*p)<<4)|g_ascii_xdigit_value(p[1]);
        g_byte_array_append(section,&byte,1);
        ++p;
    }
    g_free(hex);
    /* The assembler verifies the CRC before calling the message extractor. */
    const guint8 crc[4]={0};
    g_byte_array_append(section,crc,4);
    const char prefix[]="550e8400-e29b";
    guint8 *uuid=NULL;
    for (guint i=0;i+sizeof(prefix)<=section->len;i++)
        if (!memcmp(section->data+i,prefix,sizeof(prefix)-1)) uuid=section->data+i;
    g_assert_nonnull(uuid);

    DsmccMessages messages={0};
    json_object *status=dsmcc_messages_status(&messages);
    gint64 before=json_object_get_int64(json_object_object_get(status,"latest_cursor"));
    json_object_put(status);
    for (guint i=0;i<300;i++) {
        char unique[9];
        g_snprintf(unique,sizeof(unique),"%08x",i);
        memcpy(uuid,unique,8);
        dsmcc_messages_section(&messages,260,section->data,section->len);
    }
    now=3600LL*G_USEC_PER_SEC-1;
    dsmcc_messages_section(&messages,260,section->data,section->len);
    check_page(&messages,before,300,FALSE);
    memcpy(uuid,"abcdef01",8);
    dsmcc_messages_section(&messages,260,section->data,section->len);
    now++;
    check_page(&messages,before,1,TRUE);
    check_page(&messages,before+300,1,FALSE);
    memcpy(uuid,"0000012b",8); /* Expired UUID can be received again. */
    dsmcc_messages_section(&messages,260,section->data,section->len);
    check_page(&messages,-1,2,FALSE);
    dsmcc_messages_reset(&messages);
    check_page(&messages,before+302,0,TRUE);
    dsmcc_messages_section(&messages,260,section->data,section->len);
    check_page(&messages,-1,1,FALSE);
    now+=3600LL*G_USEC_PER_SEC;
    dsmcc_messages_expire(&messages);
    g_assert_true(g_queue_is_empty(&messages.entries));
    g_assert_cmpuint(g_hash_table_size(messages.ids),==,0);
    dsmcc_messages_clear(&messages);
    g_byte_array_unref(section);
    return 0;
}
