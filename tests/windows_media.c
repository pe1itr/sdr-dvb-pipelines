/* Exercise the packaged slideshow loaders and both speaker sample formats. */
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <alsa/asoundlib.h>
int main(int argc,char **argv) {
    g_assert_cmpint(argc,==,2);
    GdkPixbuf *picture=gdk_pixbuf_new(GDK_COLORSPACE_RGB,FALSE,8,32,24);
    gdk_pixbuf_fill(picture,0x208040ff);
    const char *formats[]={"png","jpeg"};
    for (unsigned i=0;i<2;++i) {
        char *path=g_build_filename(argv[1],formats[i],NULL);
        GError *error=NULL;
        g_assert_true(gdk_pixbuf_save(picture,path,formats[i],&error,NULL));g_assert_no_error(error);
        GdkPixbuf *loaded=gdk_pixbuf_new_from_file(path,&error);g_assert_no_error(error);g_assert_nonnull(loaded);
        g_assert_cmpint(gdk_pixbuf_get_width(loaded),==,32);
        g_assert_cmpint(gdk_pixbuf_get_height(loaded),==,24);
        const guchar *pixel=gdk_pixbuf_get_pixels(loaded);
        g_assert_cmpint(pixel[1],>,100);g_assert_cmpint(pixel[0],<,50);
        g_object_unref(loaded);g_unlink(path);g_free(path);
    }
    g_object_unref(picture);
    /* The theme's embedded SVGs need the packaged librsvg loader too. */
    gtk_init(&argc,&argv);
    const char *assets[]={"check-symbolic.svg","dash-symbolic.svg","bullet-symbolic.svg"};
    for (unsigned i=0;i<G_N_ELEMENTS(assets);++i) {
        char *resource=g_strconcat("/org/gtk/libgtk/theme/Adwaita/assets/",assets[i],NULL);
        GError *error=NULL;
        GdkPixbuf *loaded=gdk_pixbuf_new_from_resource(resource,&error);
        g_assert_no_error(error);g_assert_nonnull(loaded);
        g_assert_cmpint(gdk_pixbuf_get_width(loaded),>,0);
        g_assert_cmpint(gdk_pixbuf_get_height(loaded),>,0);
        g_object_unref(loaded);g_free(resource);
    }
    for (int format=0;format<2;++format) {
        snd_pcm_t *pcm=NULL;
        g_assert_cmpint(snd_pcm_open(&pcm,"default",SND_PCM_STREAM_PLAYBACK,SND_PCM_NONBLOCK),==,0);
        g_assert_cmpint(snd_pcm_set_params(pcm,format ? SND_PCM_FORMAT_S16_LE : SND_PCM_FORMAT_FLOAT,
            SND_PCM_ACCESS_RW_INTERLEAVED,2,48000,1,100000),==,0);
        float silence[1920]={0};
        snd_pcm_sframes_t n;
        for (unsigned tries=0;;++tries) {
            g_assert_cmpuint(tries,<,100);
            n=snd_pcm_writei(pcm,silence,960);
            if (n!=-EAGAIN) break;
            snd_pcm_wait(pcm,10);
        }
        g_assert_cmpint(n,>,0);snd_pcm_drop(pcm);snd_pcm_close(pcm);
    }
    puts("Windows PNG/JPEG slideshow, Adwaita SVG assets and float/s16 speaker output: OK");
    return 0;
}
