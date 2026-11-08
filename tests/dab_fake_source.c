/* Hardware-free native source for DAB tune/control lifecycle tests. */
#include <glib.h>
#include <glib-unix.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "tune_control.h"
int main(int argc,char **argv) {
    const char *directory=NULL;
    for (int i=1;i+1<argc;++i)
        if (!strcmp(argv[i],"--dab-state-dir")) directory=argv[++i];
    if (!directory || !g_unix_set_fd_nonblocking(0,TRUE,NULL)) return 2;
    TuneControl control={0};uint64_t written=0;
    double hz=0,gain=0,bandwidth=0;
    unsigned char iq[65536]={0};
    for (;;) {
        int changed=tune_control_read(&control,0,1,6000000000.,&hz,0,100,1,&gain,2048000,&bandwidth);
        if ((changed&1) && control.tune_generation) {
            char *path=g_build_filename(directory,"tuned",NULL);
            char *value=g_strdup_printf("%" G_GUINT64_FORMAT " %.0f %" G_GUINT64_FORMAT "\n",
                control.tune_generation,hz,written);
            gboolean ok=g_file_set_contents(path,value,-1,NULL);g_free(path);g_free(value);
            if (!ok) return 3;
        }
        ssize_t n=write(1,iq,sizeof(iq));
        if (n<=0) break;
        written+=n;g_usleep(10000);
    }
    return 0;
}
