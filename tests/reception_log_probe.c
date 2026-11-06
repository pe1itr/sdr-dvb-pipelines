#include "../src/reception_log.h"
int main(int argc, char **argv) {
    if (argc==5 && (!strcmp(argv[1],"--edit") || !strcmp(argv[1],"--stale-edit"))) {
        GError *error=NULL;
        GPtrArray *rows=reception_log_read(argv[2],&error);
        GPtrArray *replacement=rows ? reception_log_read(argv[4],&error) : NULL;
        unsigned index=(unsigned)g_ascii_strtoull(argv[3],NULL,10);
        gboolean ok=FALSE;
        if (rows && index<rows->len && replacement && replacement->len==1) {
            char **original=g_ptr_array_index(rows,index);
            if (!strcmp(argv[1],"--stale-edit")) {
                g_free(original[LOG_SERVICE]); original[LOG_SERVICE]=g_strdup("Outdated service");
            }
            ok=reception_log_replace(argv[2],index,original,g_ptr_array_index(replacement,0),&error);
        }
        if (rows) g_ptr_array_unref(rows);
        if (replacement) g_ptr_array_unref(replacement);
        if (error) { g_printerr("%s\n",error->message); g_error_free(error); }
        return ok ? 0 : 1;
    }
    if (argc!=3) return 2;
    GError *error=NULL;
    GPtrArray *rows=reception_log_read(argv[1],&error);
    gboolean ok=rows && reception_log_export(argv[2],argv[1],rows,&error);
    if (rows) g_ptr_array_unref(rows);
    if (error) { g_printerr("%s\n",error->message); g_error_free(error); }
    return ok ? 0 : 1;
}
