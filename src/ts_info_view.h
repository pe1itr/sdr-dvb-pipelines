#ifndef TS_INFO_VIEW_H
#define TS_INFO_VIEW_H
#include <gtk/gtk.h>
#include "ts_info.h"
typedef struct {
    GtkWidget *buttons[3], *dialogs[3], *services[3], *pmt, *eit, *page, *status[3];
    GtkWidget *hdmi_eit;
    GtkWidget *reveal, *hdmi_page;
    GtkWidget *page_number, *page_controls, *color_buttons[4];
    gint64 eit_utc; /* UTC seconds at the live or recorded source position */
    guint flash_timer;
    gboolean flash_on;
    TsInfoBackend *backend; /* borrowed; detached before pipeline destruction */
    TsInfoSnapshot snapshot;
    gboolean updating;
    gint64 next_tick;
} TsInfoView;
void ts_info_view_create(TsInfoView *v, GtkWindow *parent, GtkWidget *bar, GtkWidget *pmt_item);
void ts_info_view_add_eit_overlay(TsInfoView *v, GtkOverlay *overlay);
gboolean ts_info_view_eit_visible(const TsInfoView *v);
void ts_info_view_set_eit_visible(TsInfoView *v, gboolean enabled);
GtkWidget *ts_info_view_create_hdmi_page(TsInfoView *v);
void ts_info_view_tick(TsInfoView *v, TsInfoBackend *backend);
void ts_info_view_reset(TsInfoView *v);
void ts_info_view_destroy(TsInfoView *v);
#endif
