/* Exercise native GTK creation and actual embedded VLC pixels, without SDR. */
#define main application_main
#include "../src/linrad_dvb.c"
#undef main
#include "sdr_status_endpoint_test.h"

static const char *transport_path;
static gboolean checked;
static gboolean exercise(gpointer data) {
    App *a=data;
    static unsigned ticks;
    static unsigned phase;
    if (phase==1) {
        g_assert_cmpint(a->exit_code,==,0);
        if (++ticks==5) stop_pipeline(a);
        if (ticks>5 && !a->running) {
            window_close(a->window,NULL,a);
            return G_SOURCE_REMOVE;
        }
        g_assert_cmpuint(ticks,<,100);
        return G_SOURCE_CONTINUE;
    }
    if (!ticks++) {
        int fd=open(transport_path,O_RDONLY);
        g_assert_cmpint(fd,>=,0);
        GError *error=NULL;
        g_assert_true(video_view_start(&a->video_view,fd,NULL,NULL,TRUE,&error));
        g_assert_no_error(error); close(fd);
    }
    g_mutex_lock(&a->video_view.mutex);
    if (a->video_view.ready) {
        const unsigned char *pixel=a->video_view.frame+a->video_view.height/2*a->video_view.pitch+a->video_view.width/2*4;
        checked=pixel[1]>80 && pixel[2]<40;
    }
    g_mutex_unlock(&a->video_view.mutex);
    if (checked) {
        const char *path=g_getenv("LINRAD_WINDOWS_SCREENSHOT");
        if (path) {
            GdkWindow *window=gtk_widget_get_window(a->window);
            GdkPixbuf *picture=gdk_pixbuf_get_from_window(window,0,0,
                gtk_widget_get_allocated_width(a->window),gtk_widget_get_allocated_height(a->window));
            g_assert_nonnull(picture);
            g_assert_true(gdk_pixbuf_save(picture,path,"png",NULL,NULL));
            g_object_unref(picture);
        }
        video_view_stop(&a->video_view);
        int socket_fd=socket(AF_INET,SOCK_DGRAM,0);
        struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        g_assert_cmpint(bind(socket_fd,(struct sockaddr *)&address,sizeof(address)),==,0);
        socklen_t length=sizeof(address);
        g_assert_cmpint(getsockname(socket_fd,(struct sockaddr *)&address,&length),==,0);
        close(socket_fd);
        g_key_file_set_integer(a->config,"linrad","port",ntohs(address.sin_port));
        start_pipeline(a);
        g_assert_true(a->running);
        g_assert_cmpint(a->tune_fd,>=,0);
        ticks=0; phase=1;
        return G_SOURCE_CONTINUE;
    }
    g_assert_cmpuint(ticks,<,200);
    return G_SOURCE_CONTINUE;
}

int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"--test-sdr-status")) {exercise_sdr_status();return 0;}
    g_assert_cmpint(argc,==,3);
    App a={.sr=125000,.tune_fd=-1};
    a.config=g_key_file_new(); a.config_path=g_canonicalize_filename(argv[1],NULL);
    a.directory=g_path_get_dirname(a.config_path); transport_path=argv[2];
    g_assert_true(g_key_file_load_from_file(a.config,a.config_path,G_KEY_FILE_KEEP_COMMENTS,NULL));
    source_defaults(&a); load_session(&a);
    g_timeout_add(50,exercise,&a); gui(&a);
    g_assert_true(checked);
    clear_stages(&a); g_key_file_unref(a.config); g_free(a.config_path); g_free(a.directory);
    puts("Windows GTK + embedded MPEG-TS video: OK");
    return 0;
}
