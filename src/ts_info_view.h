#ifndef TS_INFO_VIEW_H
#define TS_INFO_VIEW_H
#include <gtk/gtk.h>
#include "ts_info.h"
typedef struct {
    GtkWidget *buttons[2], *dialog, *tabs, *services, *eit, *page, *status[2];
    TsInfoBackend *backend; /* borrowed; detached before pipeline destruction */
    TsInfoSnapshot snapshot;
    gboolean updating;
    gint64 next_tick;
} TsInfoView;
void ts_info_view_create(TsInfoView *v, GtkWindow *parent, GtkWidget *bar);
void ts_info_view_tick(TsInfoView *v, TsInfoBackend *backend);
void ts_info_view_reset(TsInfoView *v);
void ts_info_view_destroy(TsInfoView *v);
#endif
