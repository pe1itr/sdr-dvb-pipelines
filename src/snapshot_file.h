#ifndef SNAPSHOT_FILE_H
#define SNAPSHOT_FILE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <glib.h>
/* 1 means the old snapshot is still held open by a reader/virus scanner.
 * The caller can retry on the next update without stopping IQ reception. */
static inline int snapshot_busy(DWORD error) {
    return error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION || error==ERROR_ACCESS_DENIED;
}
#endif
/* Single writer per path. Never remove the previous snapshot before replacing
 * it: readers must always see either a complete old or complete new file. */
static inline int snapshot_write(const char *path, const void *data, size_t length) {
    size_t n=strlen(path);
    char *temporary=(char *)malloc(n+5);
    if (!temporary) { errno=ENOMEM; return -1; }
    memcpy(temporary,path,n); memcpy(temporary+n,".tmp",5);
#ifdef _WIN32
    wchar_t *wide=(wchar_t *)g_utf8_to_utf16(path,-1,NULL,NULL,NULL);
    wchar_t *tmp=(wchar_t *)g_utf8_to_utf16(temporary,-1,NULL,NULL,NULL);
    free(temporary);
    if (!wide || !tmp) { g_free(wide);g_free(tmp);errno=EINVAL;return -1; }
    HANDLE file=CreateFileW(tmp,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    DWORD error=ERROR_SUCCESS;
    if (file==INVALID_HANDLE_VALUE) error=GetLastError();
    else {
        const unsigned char *bytes=(const unsigned char *)data;
        while (length) {
            DWORD wrote=0, count=(DWORD)(length>0x10000000 ? 0x10000000 : length);
            if (!WriteFile(file,bytes,count,&wrote,NULL)) {error=GetLastError();break;}
            if (!wrote) {error=ERROR_WRITE_FAULT;break;}
            bytes+=wrote;length-=wrote;
        }
        if (!CloseHandle(file) && !error) error=GetLastError();
        if (!error && !MoveFileExW(tmp,wide,MOVEFILE_REPLACE_EXISTING)) error=GetLastError();
    }
    if (error) DeleteFileW(tmp);
    g_free(wide);g_free(tmp);
    SetLastError(error);
    if (error) {errno=snapshot_busy(error)?EAGAIN:EIO;return snapshot_busy(error)?1:-1;}
    return 0;
#else
    FILE *file=fopen(temporary,"wb");
    if (!file) {free(temporary);return -1;}
    int failed=fwrite(data,1,length,file)!=length;
    int saved=errno;
    if (fclose(file)) {failed=1;saved=errno;}
    if (!failed && rename(temporary,path)) {failed=1;saved=errno;}
    if (failed) remove(temporary);
    free(temporary);errno=saved;
    return failed?-1:0;
#endif
}
#endif
