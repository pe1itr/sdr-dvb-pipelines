#include "ts_info.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static void write_frame(const TsInfoSnapshot *s, GBytes *frame, const char *suffix) {
    const char *base=g_getenv("TELETEXT_PPM");
    if (!base || !frame) return;
    char *path=g_strconcat(base,suffix,NULL);
    FILE *file=fopen(path,"wb"); g_assert_nonnull(file); g_free(path);
    fprintf(file,"P6\n%d %d\n255\n",s->width,s->height);
    gsize n; const guint8 *data=g_bytes_get_data(frame,&n);
    for (gsize i=0;i<n;i+=4) {
        guint32 pixel; memcpy(&pixel,data+i,4);
        guint8 rgb[]={pixel>>16,pixel>>8,pixel}; fwrite(rgb,1,3,file);
    }
    fclose(file);
}

static void report(const TsInfoSnapshot *s) {
    struct json_object *out=json_object_new_object(), *events=json_object_new_array(), *services=json_object_new_array();
    json_object_object_add(out,"selected",json_object_new_int(s->selected));
    json_object_object_add(out,"pmt",json_object_new_boolean(s->pmt_seen));
    json_object_object_add(out,"teletext",json_object_new_boolean(s->teletext_announced));
    json_object_object_add(out,"decoder",json_object_new_boolean(s->teletext_decoder));
    json_object_object_add(out,"eit_received",json_object_new_int64(s->eit_received));
    json_object_object_add(out,"teletext_received",json_object_new_int64(s->teletext_received));
    json_object_object_add(out,"width",json_object_new_int(s->width));
    json_object_object_add(out,"height",json_object_new_int(s->height));
    json_object_object_add(out,"services",services);
    json_object_object_add(out,"events",events);
    for (unsigned i=0;i<s->count;i++) {
        struct json_object *service=json_object_new_object();
        json_object_object_add(service,"id",json_object_new_int(s->services[i].id));
        json_object_object_add(service,"name",json_object_new_string(s->services[i].name));
        json_object_array_add(services,service);
    }
    for (unsigned i=0;i<2;i++) {
        const TsInfoEvent *e=&s->events[i];
        struct json_object *event=json_object_new_object();
        json_object_object_add(event,"valid",json_object_new_boolean(e->valid));
        json_object_object_add(event,"title",json_object_new_string(e->title));
        json_object_object_add(event,"text",json_object_new_string(e->text));
        json_object_object_add(event,"start",json_object_new_int64(e->start));
        json_object_object_add(event,"duration",json_object_new_int(e->duration));
        json_object_array_add(events,event);
    }
    if (s->page) {
        gsize n; const guint8 *data=g_bytes_get_data(s->page,&n);
        char *hash=g_compute_checksum_for_data(G_CHECKSUM_SHA256,data,n);
        json_object_object_add(out,"page_hash",json_object_new_string(hash)); g_free(hash);
        unsigned colors=0;
        for (gsize i=0;i<n;i+=4) {
            guint32 pixel; memcpy(&pixel,data+i,4);
            if (pixel==0xff0000) colors|=1;
            if (pixel==0x00ff00) colors|=2;
            if (pixel==0x0000ff) colors|=4;
            if (pixel==0xffff00) colors|=8;
        }
        json_object_object_add(out,"colors",json_object_new_int(colors));
        write_frame(s,s->page,"");
        write_frame(s,s->page_flash_off,".off");
        write_frame(s,s->page_reveal,".reveal");
        write_frame(s,s->page_reveal_flash_off,".reveal-off");
    }
    puts(json_object_to_json_string_ext(out,JSON_C_TO_STRING_PLAIN)); json_object_put(out);
}
int main(int argc, char **argv) {
    TsInfoParser *p=ts_info_parser_new();
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--gap")) ts_info_parser_gap(p);
        else if (!strcmp(argv[i],"--reset")) { ts_info_parser_free(p); p=ts_info_parser_new(); }
        else if (g_str_has_prefix(argv[i],"--select=")) ts_info_parser_select(p,atoi(argv[i]+9));
        else {
            char *data; gsize size; g_assert_true(g_file_get_contents(argv[i],&data,&size,NULL));
            /* Vary chunk boundaries, including inside packet headers. */
            for (gsize at=0;at<size;) {
                gsize n=MIN(size-at,1+at%311);
                ts_info_parser_feed(p,(guint8 *)data+at,n); at+=n;
            }
            g_free(data);
        }
        TsInfoSnapshot s={0}; ts_info_parser_snapshot(p,&s); report(&s); ts_info_snapshot_clear(&s);
    }
    ts_info_parser_free(p); return 0;
}
