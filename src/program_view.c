#include "program_view.h"
#include <string.h>

static void selected(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
    ProgramView *v = data;
    if (v->updating || !row || !gtk_widget_is_sensitive(GTK_WIDGET(list))) return;
    int index = gtk_list_box_row_get_index(row);
    if (index >= 0 && (unsigned)index < v->count) v->select(v->services[index].id, v->data);
}

GtkWidget *program_view_create(ProgramView *v, void (*select)(guint16, gpointer), gpointer data) {
    v->select = select; v->data = data;
    v->frame = gtk_frame_new("Programs");
    gtk_widget_set_no_show_all(v->frame, TRUE);
    gtk_widget_set_size_request(v->frame, 320, 180);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    gtk_container_add(GTK_CONTAINER(v->frame), box);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    v->list = gtk_list_box_new();
    gtk_widget_set_name(v->list, "received-programs");
    atk_object_set_name(gtk_widget_get_accessible(v->list), "Received programs");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(v->list), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(scroll), v->list);
    g_signal_connect(v->list, "row-selected", G_CALLBACK(selected), v);
    v->hint = gtk_label_new("Waiting for programs");
    gtk_label_set_xalign(GTK_LABEL(v->hint), 0);
    gtk_label_set_line_wrap(GTK_LABEL(v->hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v->hint), 35);
    gtk_box_pack_start(GTK_BOX(box), v->hint, FALSE, FALSE, 0);
    gtk_widget_show_all(box);
    return v->frame;
}

void program_view_update(ProgramView *v, const TsInfoSnapshot *s, guint16 playing, gboolean enabled) {
    if (!v->frame) return;
    v->updating = TRUE;
    if (v->count != s->count || memcmp(v->services, s->services, sizeof(v->services))) {
        GList *rows = gtk_container_get_children(GTK_CONTAINER(v->list));
        for (GList *row = rows; row; row = row->next) gtk_widget_destroy(row->data);
        g_list_free(rows);
        v->count = s->count; memcpy(v->services, s->services, sizeof(v->services));
        for (unsigned i = 0; i < s->count; ++i) {
            const TsInfoService *service = &s->services[i];
            char *title = *service->name ? g_strdup(service->name) : g_strdup_printf("Program %u", service->id);
            GtkWidget *label = gtk_label_new(title);
            gtk_label_set_xalign(GTK_LABEL(label), 0);
            gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_widget_set_margin_start(label, 10); gtk_widget_set_margin_end(label, 10);
            gtk_widget_set_margin_top(label, 8); gtk_widget_set_margin_bottom(label, 8);
            gtk_list_box_insert(GTK_LIST_BOX(v->list), label, -1);
            char *tip = g_strdup_printf("%s\nService %u%s%s", title, service->id,
                *service->provider ? " · " : "", service->provider);
            gtk_widget_set_tooltip_text(gtk_widget_get_parent(label), tip);
            g_free(tip); g_free(title);
        }
        gtk_widget_show_all(v->list);
    }
    GtkListBoxRow *active = NULL;
    for (unsigned i = 0; i < s->count; ++i)
        if (s->services[i].id == playing) active = gtk_list_box_get_row_at_index(GTK_LIST_BOX(v->list), i);
    gtk_list_box_select_row(GTK_LIST_BOX(v->list), active);
    gtk_widget_set_sensitive(v->list, enabled && s->count > 0);
    const char *hint = !s->count ? "Waiting for programs" :
        enabled ? "Click a program to watch" : "Choose the embedded player in Renderer to watch a program here.";
    if (strcmp(gtk_label_get_text(GTK_LABEL(v->hint)), hint)) gtk_label_set_text(GTK_LABEL(v->hint), hint);
    v->updating = FALSE;
}
