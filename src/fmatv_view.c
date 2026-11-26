#define _POSIX_C_SOURCE 200809L
#include "fmatv_view.h"
#include "aligned_memory.h"
#include "../components/fmatv/analog/light_stack.h"
#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>

static gpointer receive_fields(gpointer opaque) {
    VideoView *v = opaque;
    const size_t size = 4*FMATV_WIDTH*FMATV_HEIGHT;
    FmatvFieldInfo *storage = g_malloc(sizeof(*storage)+size);
    uint32_t *pixels = (uint32_t *)(storage+1);
    unsigned char *packet = (unsigned char *)storage;
    LightStack *stack = NULL;
    size_t used = 0;
    while (!g_atomic_int_get(&v->stopping)) {
        struct pollfd p = {.fd=v->source_fd, .events=POLLIN};
        int rc = poll(&p,1,100);
        if (!rc || (rc<0 && errno==EINTR)) continue;
        ssize_t n = rc<0 ? -1 : read(p.fd,packet+used,sizeof(*storage)+size-used);
        if (n<0 && (errno==EAGAIN || errno==EINTR)) continue;
        if (n<=0) {
            if (n<0 || used) g_atomic_int_set(&v->relay_failed,TRUE);
            break;
        }
        used += n;
        if (used!=sizeof(*storage)+size) continue;
        g_mutex_lock(&v->mutex);
        if (v->framestack_enabled) {
            if (!stack) stack = g_new0(LightStack,1);
            if (v->framestack_reset) light_stack_reset(stack);
            light_stack_add(stack,pixels,(uint32_t *)v->frame);
        } else {
            memcpy(v->frame,pixels,size);
        }
        v->framestack_reset = FALSE;
        v->analog_info = *storage;
        ++v->analog_fields;
        v->stack_fields = v->framestack_enabled ? stack->count : 0;
        v->frame_time = g_get_monotonic_time();
        v->ready = v->dirty = TRUE;
        g_mutex_unlock(&v->mutex);
        used = 0;
    }
    g_free(stack);
    g_free(storage);
    g_atomic_int_set(&v->input_ended,TRUE);
    return NULL;
}

gboolean fmatv_view_start(VideoView *v, int fd, GError **error) {
    video_view_stop(v);
    v->source_fd = fcntl(fd,F_DUPFD_CLOEXEC,3);
    v->frame = linrad_aligned_alloc(32,4*FMATV_WIDTH*FMATV_HEIGHT);
    if (v->source_fd<0 || !v->frame) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_FAILED,"Cannot open FM-ATV video display");
        video_view_stop(v);
        return FALSE;
    }
    v->width=FMATV_WIDTH; v->height=FMATV_HEIGHT; v->pitch=4*v->width;
    v->aspect=4./3; v->active=v->local_display=v->analog=TRUE;
    v->analog_info=(FmatvFieldInfo){0};
    v->analog_fields=v->stack_fields=0;
    g_atomic_int_set(&v->stopping,FALSE);
    g_atomic_int_set(&v->input_ended,FALSE);
    g_atomic_int_set(&v->relay_failed,FALSE);
    v->relay=g_thread_new("fm-atv-display",receive_fields,v);
    return TRUE;
}

static const char *const lamp_names[] = {
    "Sync","Color burst","Frame stacking","Audio 6 MHz","Audio 7.02 MHz",
    "Audio 7.22 MHz","NICAM","Audio 7.38 MHz","Audio 7.56 MHz"
};
static const unsigned lamp_flags[] = {
    FMATV_SYNC,FMATV_BURST,FMATV_STACK,FMATV_MONO,FMATV_AUDIO_702,
    FMATV_AUDIO_722,FMATV_NICAM,FMATV_AUDIO_738,FMATV_AUDIO_756
};

void fmatv_service_create(FmatvService *service,GtkWidget *frame) {
    GtkWidget *digital=gtk_bin_get_child(GTK_BIN(frame));
    g_object_ref(digital);
    gtk_container_remove(GTK_CONTAINER(frame),digital);
    service->stack=gtk_stack_new();
    /* Service info also sets the video panel size. Reserve both pages in either mode. */
    gtk_stack_set_homogeneous(GTK_STACK(service->stack),TRUE);
    gtk_container_add(GTK_CONTAINER(frame),service->stack);
    gtk_stack_add_named(GTK_STACK(service->stack),digital,"digital");
    g_object_unref(digital);
    GtkWidget *grid=gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid),8);
    gtk_grid_set_column_spacing(GTK_GRID(grid),10);
    gtk_grid_set_row_spacing(GTK_GRID(grid),5);
    gtk_stack_add_named(GTK_STACK(service->stack),grid,"fmatv");
    for (unsigned i=0;i<G_N_ELEMENTS(lamp_names);++i) {
        GtkWidget *lamp=service->lamps[i]=gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(lamp),0);
        gtk_grid_attach(GTK_GRID(grid),lamp,i%3,i/3,1,1);
    }
    gtk_widget_set_tooltip_text(service->lamps[2],"Green after six PAL fields have entered the rolling stack");
    gtk_widget_set_tooltip_text(service->lamps[6],"Green on validated NICAM frame lock at 6.552 MHz");
    const char *names[]={"Video SNR","Line frequency","Raster frequency","Frame count"};
    for (unsigned i=0;i<G_N_ELEMENTS(names);++i) {
        GtkWidget *name=gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(name),0);
        gtk_grid_attach(GTK_GRID(grid),name,0,3+i,1,1);
        GtkWidget *value=service->values[i]=gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(value),0);
        gtk_grid_attach(GTK_GRID(grid),value,1,3+i,2,1);
    }
    gtk_widget_set_tooltip_text(service->values[3],"Received PAL fields since the receiver started");
    GtkWidget *mode=gtk_label_new("FM detector");
    gtk_label_set_xalign(GTK_LABEL(mode),0);
    gtk_grid_attach(GTK_GRID(grid),mode,0,7,1,1);
    GtkWidget *detector=gtk_label_new("FM0");
    gtk_label_set_xalign(GTK_LABEL(detector),0);
    gtk_grid_attach(GTK_GRID(grid),detector,1,7,2,1);
    fmatv_service_update(service,0,0,NULL,0,0);
}

void fmatv_service_update(FmatvService *service,unsigned enabled,unsigned detected,
                          const FmatvFieldInfo *field,guint64 count,unsigned stacked) {
    if (field && stacked==LIGHT_STACK_FRAMES) detected|=FMATV_STACK;
    for (unsigned i=0;i<G_N_ELEMENTS(lamp_names);++i) {
        const char *color=!(enabled & lamp_flags[i]) ? "#888888" :
            (detected & lamp_flags[i]) ? "#36d866" : "#4098ef";
        char markup[128];
        g_snprintf(markup,sizeof(markup),"<span foreground=\"%s\">●</span> %s",color,lamp_names[i]);
        if (lamp_flags[i]==FMATV_STACK) {
            size_t used=strlen(markup);
            g_snprintf(markup+used,sizeof(markup)-used," %u/%u",stacked,(unsigned)LIGHT_STACK_FRAMES);
        }
        GtkLabel *label=GTK_LABEL(service->lamps[i]);
        if (strcmp(gtk_label_get_label(label),markup)) gtk_label_set_markup(label,markup);
    }
    const double values[]={field ? field->snr_db : NAN,field ? field->line_hz : NAN,
                           field ? field->raster_hz : NAN};
    for (unsigned i=0;i<G_N_ELEMENTS(service->values);++i) {
        char buf[64]="—";
        if (i==3) g_snprintf(buf,sizeof(buf),"%" G_GUINT64_FORMAT,count);
        else if (isfinite(values[i])) g_snprintf(buf,sizeof(buf),i==0 ? "%.1f dB" : "%.2f Hz",values[i]);
        GtkLabel *label=GTK_LABEL(service->values[i]);
        if (strcmp(gtk_label_get_text(label),buf)) gtk_label_set_text(label,buf);
    }
}
