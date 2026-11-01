#ifndef PROGRAM_VIEW_H
#define PROGRAM_VIEW_H
#include <gtk/gtk.h>
#include "ts_info.h"

typedef struct {
    GtkWidget *frame, *list, *hint;
    TsInfoService services[TS_INFO_SERVICES];
    unsigned count;
    gboolean updating;
    void (*select)(guint16 service, gpointer data);
    gpointer data;
} ProgramView;

GtkWidget *program_view_create(ProgramView *v, void (*select)(guint16, gpointer), gpointer data);
void program_view_update(ProgramView *v, const TsInfoSnapshot *s, guint16 playing, gboolean enabled);
#endif
