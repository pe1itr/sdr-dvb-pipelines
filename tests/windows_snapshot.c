#include "snapshot_file.h"
#include <assert.h>
#include <glib/gstdio.h>
int main(int argc,char **argv) {
#ifdef _WIN32
    /* CRT argv uses the ANSI code page; control/snapshot paths are UTF-8. */
    argv=g_win32_get_command_line();
#endif
    assert(argc==2);
    char *path=g_build_filename(argv[1],"state-\xc3\xa9-\xe6\x97\xa5.json",NULL);
    assert(snapshot_write(path,"old",3)==0);
    wchar_t *wide=(wchar_t *)g_utf8_to_utf16(path,-1,NULL,NULL,NULL);
    HANDLE reader=CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    assert(reader!=INVALID_HANDLE_VALUE);
    for (int k=0;k<20;++k) assert(snapshot_write(path,"new",3)==1);
    char data[4]={0};DWORD n=0;
    assert(ReadFile(reader,data,3,&n,NULL) && n==3 && !strcmp(data,"old"));
    CloseHandle(reader);
    assert(snapshot_write(path,"new",3)==0);
    char *contents=NULL;gsize length=0;
    assert(g_file_get_contents(path,&contents,&length,NULL) && length==3 && !strcmp(contents,"new"));
    g_free(contents);
    /* A missing directory is a real failure, rather than a sharing retry. */
    char *missing=g_build_filename(argv[1],"absent","state.json",NULL);
    assert(snapshot_write(missing,"{}",2)==-1);
    g_free(missing);g_unlink(path);g_free(wide);g_free(path);
    return 0;
}
