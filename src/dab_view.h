#ifndef DAB_VIEW_H
#define DAB_VIEW_H
#include <gtk/gtk.h>
#include <json-c/json.h>
#include "decoder_view.h"
typedef struct {
    char ensemble[128], tii[16];
    double frequency;
    gint64 observed;
} DabReception;
typedef struct {
    GtkWidget *info, *label, *image_frame, *image, *caption, *stations_frame, *stations;
    GtkWidget *tii_button, *tii_dialog, *tii_rows;
    GtkWindow *parent;
    char *directory, *last_json, *station_signature, *tii_signature;
    json_object *snapshot;
    char *pending_json;
    GString *status_line;
    gboolean status_discard;
    gint64 snapshot_time;
    guint status_watch;
    int status_fd;
    unsigned slide;
    guint64 generation, scope_sequence;
    DecoderView *decoder;
    gboolean active;
    void (*log)(const DabReception*,gpointer);
    GHashTable *(*logged_tii)(const char *ensemble,gpointer);
    gpointer data;
} DabView;
void dab_view_create(DabView *v,DecoderView *decoder,GtkWindow *parent,void (*log)(const DabReception*,gpointer),GHashTable *(*logged_tii)(const char*,gpointer),gpointer data);
void dab_view_refresh_tii(DabView *v);
gboolean dab_view_retune(DabView *v,int fd,const char *channel,guint64 hz,guint64 tuner_hz);
void dab_view_mute(DabView *v,gboolean muted);
void dab_view_reset(DabView *v);
void dab_view_feed(DabView *v,const char *data,gsize length);
void dab_view_watch(DabView *v,int fd);
void dab_view_tick(DabView *v,gboolean active);
void dab_view_destroy(DabView *v);
const char *dab_view_text(DabView *v,const char *key);
double dab_view_number(DabView *v,const char *key);
#endif
