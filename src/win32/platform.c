/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LINRAD_WIN32_IMPLEMENTATION
#define LINRAD_WIN32_IMPLEMENTATION
#endif
#include "platform.h"
#include <limits.h>

/* Socket descriptors are kept separate from CRT descriptors. Never truncate a
 * SOCKET to int or close one with the CRT's _close(). */
#define SOCKET_BASE 65536
#define SOCKET_COUNT 4096
static SOCKET sockets[SOCKET_COUNT];
static SRWLOCK socket_lock = SRWLOCK_INIT;
static unsigned char nonblocking[8192];
static HANDLE process_job;

static int win_error(DWORD code) {
    switch (code) {
    case WSAEWOULDBLOCK: case ERROR_NO_DATA: errno=EAGAIN; break;
    case WSAEINTR: errno=EINTR; break;
    case WSAEADDRINUSE: errno=EADDRINUSE; break;
    case WSAECONNRESET: case WSAECONNABORTED: case ERROR_BROKEN_PIPE: errno=EPIPE; break;
    case WSAETIMEDOUT: errno=ETIMEDOUT; break;
    case WSAEINVAL: case ERROR_INVALID_PARAMETER: errno=EINVAL; break;
    case WSAENOTSOCK: case ERROR_INVALID_HANDLE: errno=EBADF; break;
    default: errno=EIO; break;
    }
    return -1;
}
static SOCKET native_socket(int fd) {
    if (fd<SOCKET_BASE || fd>=SOCKET_BASE+SOCKET_COUNT) return INVALID_SOCKET;
    return sockets[fd-SOCKET_BASE];
}
static int register_socket(SOCKET socket) {
    if (socket==INVALID_SOCKET) return win_error(WSAGetLastError());
    SetHandleInformation((HANDLE)socket,HANDLE_FLAG_INHERIT,0);
    AcquireSRWLockExclusive(&socket_lock);
    for (int i=0;i<SOCKET_COUNT;++i) if (sockets[i]==INVALID_SOCKET) {
        sockets[i]=socket; ReleaseSRWLockExclusive(&socket_lock); return SOCKET_BASE+i;
    }
    ReleaseSRWLockExclusive(&socket_lock); closesocket(socket); errno=EMFILE; return -1;
}
static void __attribute__((constructor)) initialize(void) {
    WSADATA data; WSAStartup(MAKEWORD(2,2),&data);
    for (int i=0;i<SOCKET_COUNT;++i) sockets[i]=INVALID_SOCKET;
    _set_fmode(_O_BINARY);
    /* FreeType avoids the Win32 Pango glyph rendering differences between
     * Windows versions and Wine. Keep an explicit user override possible. */
    g_setenv("PANGOCAIRO_BACKEND","fontconfig",FALSE);
    for (int fd=0;fd<3;++fd) if (_get_osfhandle(fd)!=-1) _setmode(fd,_O_BINARY);
    char *executable=lw_file_read_link("/proc/self/exe",NULL);
    if (executable) {
        char *directory=g_path_get_dirname(executable);
        const char *fonts[]={"DejaVuSans.ttf","DejaVuSans-Bold.ttf","DejaVuSansMono.ttf","DejaVuSansMono-Bold.ttf"};
        for (unsigned i=0;i<G_N_ELEMENTS(fonts);++i) {
            char *font=g_build_filename(directory,"share/fonts/TTF",fonts[i],NULL);
            wchar_t *wide=(wchar_t *)g_utf8_to_utf16(font,-1,NULL,NULL,NULL);
            if (wide) { AddFontResourceExW(wide,FR_PRIVATE,NULL); g_free(wide); }
            g_free(font);
        }
        const char *keys[]={"VLC_PLUGIN_PATH","SOAPY_SDR_PLUGIN_PATH","GSETTINGS_SCHEMA_DIR","FONTCONFIG_PATH","GDK_PIXBUF_MODULEDIR"};
        const char *paths[]={"plugins","lib/SoapySDR/modules0.8","share/glib-2.0/schemas","etc/fonts","lib/gdk-pixbuf-2.0/2.10.0/loaders"};
        for (unsigned i=0;i<G_N_ELEMENTS(keys);++i) {
            char *path=g_build_filename(directory,paths[i],NULL);
            if (g_file_test(path,G_FILE_TEST_IS_DIR)) g_setenv(keys[i],path,TRUE);
            g_free(path);
        }
        char *loaders=g_build_filename(directory,"lib/gdk-pixbuf-2.0/2.10.0/loaders.cache",NULL);
        if (g_file_test(loaders,G_FILE_TEST_IS_REGULAR)) g_setenv("GDK_PIXBUF_MODULE_FILE",loaders,TRUE);
        g_free(loaders);
        char *path=g_strconcat(directory,";",g_getenv("PATH") ? g_getenv("PATH") : "",NULL);
        g_setenv("PATH",path,TRUE); g_free(path);
        g_free(directory); g_free(executable);
    }
    /* A worker owns a nested job. Terminating a decoder also closes its job
     * handle and kills all LDPC/player children. The handle is not inherited. */
    process_job=CreateJobObjectW(NULL,NULL);
    if (process_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(process_job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)) ||
            !AssignProcessToJobObject(process_job,GetCurrentProcess())) {
            CloseHandle(process_job); process_job=NULL;
        }
    }
}
int lw_sigaction(int sig,const struct sigaction *action,struct sigaction *old) {
    if (sig==SIGPIPE) return 0;
    void (*previous)(int)=signal(sig,action->sa_handler);
    if (old) old->sa_handler=previous;
    return previous==SIG_ERR ? -1 : 0;
}
int lw_open(const char *path,int flags,...) {
    int mode=0600;
    if (flags&O_CREAT) { va_list args; va_start(args,flags); mode=va_arg(args,int); va_end(args); }
    if (!strcmp(path,"/dev/null")) path="NUL";
    wchar_t *wide=(wchar_t *)g_utf8_to_utf16(path,-1,NULL,NULL,NULL);
    if (!wide) { errno=EINVAL; return -1; }
    int fd=_wopen(wide,(flags&~O_NONBLOCK)|_O_BINARY,mode); g_free(wide);
    if (fd>=0 && fd<(int)sizeof(nonblocking)) nonblocking[fd]=!!(flags&O_NONBLOCK);
    return fd;
}
int lw_close(int fd) {
    SOCKET s=native_socket(fd);
    if (s!=INVALID_SOCKET) {
        AcquireSRWLockExclusive(&socket_lock); sockets[fd-SOCKET_BASE]=INVALID_SOCKET;
        ReleaseSRWLockExclusive(&socket_lock);
        return closesocket(s)==0 ? 0 : win_error(WSAGetLastError());
    }
    if (fd>=0 && fd<(int)sizeof(nonblocking)) nonblocking[fd]=0;
    return _close(fd);
}
ssize_t lw_read(int fd,void *buffer,size_t size) {
    if (native_socket(fd)!=INVALID_SOCKET) return lw_recv(fd,buffer,size,0);
    HANDLE handle=(HANDLE)_get_osfhandle(fd);
    if (fd>=0 && fd<(int)sizeof(nonblocking) && nonblocking[fd] && GetFileType(handle)==FILE_TYPE_PIPE) {
        DWORD available=0;
        if (!PeekNamedPipe(handle,NULL,0,NULL,&available,NULL)) {
            if (GetLastError()==ERROR_BROKEN_PIPE) return 0;
            return win_error(GetLastError());
        }
        if (!available) { errno=EAGAIN; return -1; }
        if (size>available) size=available;
    }
    return _read(fd,buffer,(unsigned)(size>INT_MAX ? INT_MAX : size));
}
ssize_t lw_write(int fd,const void *buffer,size_t size) {
    if (native_socket(fd)!=INVALID_SOCKET) return lw_send(fd,buffer,size,0);
    if (!size) return 0;
    HANDLE handle=(HANDLE)_get_osfhandle(fd);
    if (GetFileType(handle)==FILE_TYPE_PIPE) {
        DWORD written=0;
        if (!WriteFile(handle,buffer,(DWORD)(size>INT_MAX ? INT_MAX : size),&written,NULL)) return win_error(GetLastError());
        if (!written) { errno=EAGAIN; return -1; }
        return written;
    }
    return _write(fd,buffer,(unsigned)(size>INT_MAX ? INT_MAX : size));
}
int lw_fcntl(int fd,int command,...) {
    if (command==F_GETFL) return fd>=0 && fd<(int)sizeof(nonblocking) && nonblocking[fd] ? O_NONBLOCK : 0;
    va_list args; va_start(args,command); int value=va_arg(args,int); va_end(args);
    SOCKET s=native_socket(fd);
    if (command==F_SETFL) {
        if (s!=INVALID_SOCKET) { u_long mode=!!(value&O_NONBLOCK); return ioctlsocket(s,FIONBIO,&mode)==0 ? 0 : win_error(WSAGetLastError()); }
        HANDLE handle=(HANDLE)_get_osfhandle(fd);
        if (handle==INVALID_HANDLE_VALUE) { errno=EBADF; return -1; }
        if (GetFileType(handle)==FILE_TYPE_PIPE) {
            DWORD mode=value&O_NONBLOCK ? PIPE_NOWAIT : PIPE_WAIT;
            if (!SetNamedPipeHandleState(handle,&mode,NULL,NULL)) return win_error(GetLastError());
        }
        if (fd<(int)sizeof(nonblocking)) nonblocking[fd]=!!(value&O_NONBLOCK);
        return 0;
    }
    if (command==F_SETFD) {
        HANDLE handle=s!=INVALID_SOCKET ? (HANDLE)s : (HANDLE)_get_osfhandle(fd);
        return SetHandleInformation(handle,HANDLE_FLAG_INHERIT,value&FD_CLOEXEC ? 0 : HANDLE_FLAG_INHERIT) ? 0 : win_error(GetLastError());
    }
    if (command==F_DUPFD_CLOEXEC) {
        int duplicate=_dup(fd);
        if (duplicate>=0) {
            SetHandleInformation((HANDLE)_get_osfhandle(duplicate),HANDLE_FLAG_INHERIT,0);
            if (duplicate<(int)sizeof(nonblocking) && fd<(int)sizeof(nonblocking)) nonblocking[duplicate]=nonblocking[fd];
        }
        return duplicate;
    }
    if (command==F_SETPIPE_SZ) return value;
    errno=EINVAL; return -1;
}
int lw_pipe2(int fds[2],int flags) {
    (void)flags;
    if (_pipe(fds,1024*1024,_O_BINARY|_O_NOINHERIT)) return -1;
    if (fds[0]<(int)sizeof(nonblocking)) nonblocking[fds[0]]=0;
    if (fds[1]<(int)sizeof(nonblocking)) nonblocking[fds[1]]=0;
    return 0;
}
int lw_poll(struct linrad_pollfd *fds,nfds_t count,int timeout) {
    ULONGLONG start=GetTickCount64();
    do {
        int ready=0;
        for (nfds_t i=0;i<count;++i) {
            struct linrad_pollfd *p=&fds[i]; p->revents=0;
            if (p->fd<0 || !p->events) continue;
            SOCKET socket=native_socket(p->fd);
            if (socket!=INVALID_SOCKET) {
                WSAPOLLFD native={socket,p->events,0};
                int rc=WSAPoll(&native,1,0);
                p->revents=rc<0 ? POLLERR : native.revents;
            } else {
                HANDLE handle=(HANDLE)_get_osfhandle(p->fd);
                if (handle==INVALID_HANDLE_VALUE) p->revents=POLLNVAL;
                else if (GetFileType(handle)!=FILE_TYPE_PIPE) p->revents=p->events;
                else if (p->events&POLLIN) {
                    DWORD available=0;
                    if (!PeekNamedPipe(handle,NULL,0,NULL,&available,NULL)) p->revents=POLLHUP;
                    else if (available) p->revents=POLLIN;
                } else if (p->events&POLLOUT) p->revents=POLLOUT;
            }
            if (p->revents) ++ready;
        }
        if (ready || timeout==0) return ready;
        Sleep(1);
    } while (timeout<0 || GetTickCount64()-start<(ULONGLONG)timeout);
    return 0;
}
int lw_socket(int family,int type,int protocol) {
    int fd=register_socket(socket(family,type&~(SOCK_NONBLOCK|SOCK_CLOEXEC),protocol));
    if (fd>=0 && (type&SOCK_NONBLOCK) && lw_fcntl(fd,F_SETFL,O_NONBLOCK)<0) { lw_close(fd); return -1; }
    return fd;
}
#define SOCKET_RESULT(call) ((call)==0 ? 0 : win_error(WSAGetLastError()))
int lw_bind(int fd,const struct sockaddr *a,int n) { return SOCKET_RESULT(bind(native_socket(fd),a,n)); }
int lw_connect(int fd,const struct sockaddr *a,int n) { return SOCKET_RESULT(connect(native_socket(fd),a,n)); }
int lw_listen(int fd,int n) { return SOCKET_RESULT(listen(native_socket(fd),n)); }
int lw_accept(int fd,struct sockaddr *a,int *n) { return register_socket(accept(native_socket(fd),a,n)); }
int lw_getsockname(int fd,struct sockaddr *a,int *n) { return SOCKET_RESULT(getsockname(native_socket(fd),a,n)); }
int lw_getsockopt(int fd,int level,int option,void *v,int *n) { return SOCKET_RESULT(getsockopt(native_socket(fd),level,option,v,n)); }
int lw_setsockopt(int fd,int level,int option,const void *v,int n) {
    DWORD timeout;
    if (level==SOL_SOCKET && (option==SO_RCVTIMEO || option==SO_SNDTIMEO) && n==(int)sizeof(struct timeval)) {
        const struct timeval *t=v; timeout=t->tv_sec*1000+t->tv_usec/1000; v=&timeout; n=sizeof(timeout);
    }
    return SOCKET_RESULT(setsockopt(native_socket(fd),level,option,v,n));
}
static int socket_flags(int flags) { return flags&~(MSG_DONTWAIT|MSG_NOSIGNAL|MSG_TRUNC); }
ssize_t lw_send(int fd,const void *b,size_t n,int flags) {
    int rc=send(native_socket(fd),b,(int)n,socket_flags(flags));
    return rc==SOCKET_ERROR ? win_error(WSAGetLastError()) : rc;
}
ssize_t lw_sendto(int fd,const void *b,size_t n,int flags,const struct sockaddr *a,int length) {
    int rc=sendto(native_socket(fd),b,(int)n,socket_flags(flags),a,length);
    return rc==SOCKET_ERROR ? win_error(WSAGetLastError()) : rc;
}
ssize_t lw_recv(int fd,void *b,size_t n,int flags) { return lw_recvfrom(fd,b,n,flags,NULL,NULL); }
ssize_t lw_recvfrom(int fd,void *b,size_t n,int flags,struct sockaddr *a,int *length) {
    SOCKET socket=native_socket(fd);
    if (flags&MSG_DONTWAIT) { WSAPOLLFD p={socket,POLLIN,0}; if (WSAPoll(&p,1,0)==0) { errno=EAGAIN; return -1; } }
    int rc=recvfrom(socket,b,(int)n,socket_flags(flags),a,length);
    if (rc==SOCKET_ERROR && WSAGetLastError()==WSAEMSGSIZE && (flags&MSG_TRUNC)) return n+1;
    return rc==SOCKET_ERROR ? win_error(WSAGetLastError()) : rc;
}
void *lw_mmap(void *address,size_t size,int protection,int flags,int fd,off_t offset) {
    (void)address; (void)protection; (void)flags;
    HANDLE mapping=CreateFileMappingW((HANDLE)_get_osfhandle(fd),NULL,PAGE_READWRITE,0,0,NULL);
    if (!mapping) { win_error(GetLastError()); return MAP_FAILED; }
    void *p=MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,0,(DWORD)offset,size);
    CloseHandle(mapping);
    if (!p) { win_error(GetLastError()); return MAP_FAILED; }
    return p;
}
int lw_munmap(void *address,size_t size) { (void)size; return UnmapViewOfFile(address) ? 0 : win_error(GetLastError()); }
char *lw_file_read_link(const char *path,GError **error) {
    if (strcmp(path,"/proc/self/exe")) { g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Unsupported link"); return NULL; }
    wchar_t wide[32768]; DWORD n=GetModuleFileNameW(NULL,wide,G_N_ELEMENTS(wide));
    if (!n || n>=G_N_ELEMENTS(wide)) return NULL;
    char *result=g_utf16_to_utf8((gunichar2 *)wide,n,NULL,NULL,error);
    if (result) for (char *p=result;*p;++p) if (*p=='\\') *p='/';
    return result;
}
ssize_t lw_readlink(const char *path,char *buffer,size_t size) {
    char *name=lw_file_read_link(path,NULL); if (!name) { errno=ENOENT; return -1; }
    size_t n=strlen(name); if (n>size) n=size;
    memcpy(buffer,name,n); g_free(name); return n;
}
void lw_stop_process(GPid process) { if (process) TerminateProcess(process,1); }
GPid lw_waitpid(GPid process,int *status,int options) {
    DWORD result=WaitForSingleObject(process,options&WNOHANG ? 0 : INFINITE);
    if (result==WAIT_TIMEOUT) return NULL;
    if (result!=WAIT_OBJECT_0) { errno=ECHILD; return (GPid)(intptr_t)-1; }
    DWORD code; GetExitCodeProcess(process,&code); if (status) *status=(int)code; return process;
}
gboolean lw_open_pipe(int fds[2],int flags,GError **error) {
    if (!lw_pipe2(fds,flags)) return TRUE;
    g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),"Cannot create pipe: %s",g_strerror(errno)); return FALSE;
}
gboolean lw_set_nonblocking(int fd,gboolean enabled,GError **error) {
    if (!lw_fcntl(fd,F_SETFL,enabled ? O_NONBLOCK : 0)) return TRUE;
    g_set_error(error,G_FILE_ERROR,g_file_error_from_errno(errno),"Cannot set pipe mode: %s",g_strerror(errno)); return FALSE;
}
typedef struct { int fd; GIOCondition condition; gboolean (*callback)(int,GIOCondition,gpointer); gpointer data; } Watch;
static gboolean watch_tick(gpointer data) {
    Watch *watch=data; struct linrad_pollfd fd={watch->fd,POLLIN,0};
    if (lw_poll(&fd,1,0)<=0) return G_SOURCE_CONTINUE;
    GIOCondition condition=0;
    if (fd.revents&POLLIN) condition|=G_IO_IN;
    if (fd.revents&POLLHUP) condition|=G_IO_HUP;
    if (fd.revents&(POLLERR|POLLNVAL)) condition|=G_IO_ERR;
    return watch->callback(watch->fd,condition,watch->data);
}
guint lw_fd_add(int fd,GIOCondition condition,gboolean (*callback)(int,GIOCondition,gpointer),gpointer data) {
    Watch *watch=g_new(Watch,1); *watch=(Watch){fd,condition,callback,data};
    return g_timeout_add_full(G_PRIORITY_DEFAULT,10,watch_tick,watch,g_free);
}
static volatile sig_atomic_t pending_signal;
static void handle_signal(int sig) { pending_signal=sig; }
typedef struct { int sig; GSourceFunc callback; gpointer data; } SignalWatch;
static gboolean signal_tick(gpointer data) {
    SignalWatch *watch=data;
    if (pending_signal!=watch->sig) return G_SOURCE_CONTINUE;
    pending_signal=0; return watch->callback(watch->data);
}
guint lw_signal_add(int sig,GSourceFunc callback,gpointer data) {
    SignalWatch *watch=g_new(SignalWatch,1); *watch=(SignalWatch){sig,callback,data};
    signal(sig,handle_signal); return g_timeout_add_full(G_PRIORITY_DEFAULT,50,signal_tick,watch,g_free);
}
void *lw_aligned_alloc(size_t alignment,size_t size) { return _aligned_malloc(size,alignment); }
void lw_aligned_free(void *p) { _aligned_free(p); }
int lw_posix_memalign(void **p,size_t alignment,size_t size) { *p=lw_aligned_alloc(alignment,size); return *p ? 0 : ENOMEM; }

/* Use an explicit HANDLE_LIST instead of CRT descriptor inheritance. This
 * works with non-inheritable pipes and concurrent LDPC worker launches; only
 * the three duplicated standard handles reach the child. */
static void append_argument(GString *command,const char *argument) {
    if (command->len) g_string_append_c(command,' ');
    g_string_append_c(command,'"');
    unsigned slashes=0;
    for (const char *p=argument;;++p) {
        if (*p=='\\') { ++slashes; continue; }
        unsigned count=slashes*((*p=='"' || !*p) ? 2 : 1);
        for (unsigned i=0;i<count;++i) g_string_append_c(command,'\\');
        slashes=0;
        if (!*p) break;
        if (*p=='"') g_string_append_c(command,'\\');
        g_string_append_c(command,*p);
    }
    g_string_append_c(command,'"');
}
gboolean lw_spawn_fds(const gchar *directory,gchar **argv,gchar **env,GSpawnFlags flags,
    GSpawnChildSetupFunc setup,gpointer data,GPid *pid,gint input,gint output,gint error_fd,GError **error) {
    (void)setup; (void)data;
    gboolean ok=FALSE;
    DWORD failure=ERROR_INVALID_PARAMETER;
    HANDLE handles[3]={NULL,NULL,NULL};
    int fds[3]={input,output,error_fd};
    STARTUPINFOEXW startup={0}; startup.StartupInfo.cb=sizeof(startup);
    PROCESS_INFORMATION process={0};
    GString *command=g_string_new(NULL);
    for (unsigned i=0;argv[i];++i) append_argument(command,argv[i]);
    char *program=flags&G_SPAWN_SEARCH_PATH ? g_find_program_in_path(argv[0]) : g_strdup(argv[0]);
    wchar_t *wide_program=program ? (wchar_t *)g_utf8_to_utf16(program,-1,NULL,NULL,NULL) : NULL;
    wchar_t *wide_command=(wchar_t *)g_utf8_to_utf16(command->str,-1,NULL,NULL,NULL);
    wchar_t *wide_directory=directory ? (wchar_t *)g_utf8_to_utf16(directory,-1,NULL,NULL,NULL) : NULL;
    wchar_t *wide_env=NULL;
    if (env) {
        GArray *block=g_array_new(FALSE,TRUE,sizeof(wchar_t));
        for (unsigned i=0;env[i];++i) {
            glong length;
            gunichar2 *entry=g_utf8_to_utf16(env[i],-1,NULL,&length,NULL);
            if (!entry) { g_array_unref(block); goto done; }
            g_array_append_vals(block,entry,length+1); g_free(entry);
        }
        wchar_t zero=0; g_array_append_val(block,zero);
        if (block->len==1) g_array_append_val(block,zero);
        wide_env=(wchar_t *)g_array_free(block,FALSE);
    }
    if (!wide_program || !wide_command || (directory && !wide_directory)) goto done;
    for (int i=0;i<3;++i) {
        HANDLE original=fds[i]<0 ? GetStdHandle(i==0 ? STD_INPUT_HANDLE : i==1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE)
                                : (HANDLE)_get_osfhandle(fds[i]);
        if (!DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&handles[i],0,TRUE,DUPLICATE_SAME_ACCESS)) {
            failure=GetLastError(); goto done;
        }
    }
    SIZE_T bytes=0;
    InitializeProcThreadAttributeList(NULL,1,0,&bytes);
    startup.lpAttributeList=g_malloc(bytes);
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes)) {
        failure=GetLastError(); g_free(startup.lpAttributeList); startup.lpAttributeList=NULL; goto done;
    }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  handles,sizeof(handles),NULL,NULL)) { failure=GetLastError(); goto done; }
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=handles[0]; startup.StartupInfo.hStdOutput=handles[1]; startup.StartupInfo.hStdError=handles[2];
    if (!CreateProcessW(wide_program,wide_command,NULL,NULL,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,
        wide_env,wide_directory,&startup.StartupInfo,&process)) { failure=GetLastError(); goto done; }
    CloseHandle(process.hThread);
    if (pid) *pid=process.hProcess; else CloseHandle(process.hProcess);
    ok=TRUE;
done:
    if (!ok) {
        char *message=g_win32_error_message(failure);
        g_set_error(error,G_SPAWN_ERROR,G_SPAWN_ERROR_FAILED,"Cannot start %s: %s",argv[0],message); g_free(message);
    }
    if (startup.lpAttributeList) { DeleteProcThreadAttributeList(startup.lpAttributeList); g_free(startup.lpAttributeList); }
    for (int i=0;i<3;++i) if (handles[i]) CloseHandle(handles[i]);
    g_free(wide_program); g_free(wide_command); g_free(wide_directory); g_free(wide_env);
    g_free(program); g_string_free(command,TRUE);
    return ok;
}
gboolean lw_spawn_pipes(const gchar *directory,gchar **argv,gchar **env,GSpawnFlags flags,
    GSpawnChildSetupFunc setup,gpointer data,GPid *pid,gint *input,gint *output,gint *error_fd,GError **error) {
    int pipes[3][2]={{-1,-1},{-1,-1},{-1,-1}};
    int *requested[]={input,output,error_fd};
    int fds[]={0,1,2}; int nullfd=-1;
    gboolean ok=FALSE;
    for (int i=0;i<3;++i) if (requested[i]) {
        *requested[i]=-1;
        if (!lw_open_pipe(pipes[i],FD_CLOEXEC,error)) goto done;
        fds[i]=pipes[i][i==0 ? 0 : 1];
    }
    if ((flags&G_SPAWN_STDOUT_TO_DEV_NULL) && !output) {
        nullfd=lw_open("NUL",O_WRONLY|O_CLOEXEC); if (nullfd<0) goto done; fds[1]=nullfd;
    }
    ok=lw_spawn_fds(directory,argv,env,flags,setup,data,pid,fds[0],fds[1],fds[2],error);
    if (ok) for (int i=0;i<3;++i) if (requested[i]) {
        int side=i==0 ? 1 : 0;
        *requested[i]=pipes[i][side]; pipes[i][side]=-1;
    }
done:
    for (int i=0;i<3;++i) for (int j=0;j<2;++j) if (pipes[i][j]>=0) lw_close(pipes[i][j]);
    if (nullfd>=0) lw_close(nullfd);
    return ok;
}
