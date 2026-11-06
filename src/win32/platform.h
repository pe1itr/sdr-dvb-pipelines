/* SPDX-License-Identifier: GPL-3.0-only
 * Native Windows implementations of the small POSIX I/O surface used by the
 * receiver. Included only by the Windows build, before system headers.
 */
#ifndef LINRAD_WIN32_PLATFORM_H
#define LINRAD_WIN32_PLATFORM_H
#ifndef _WIN32
#error This header belongs to the native Windows build
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <glib.h>
#ifdef __cplusplus
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <chrono>
extern "C" {
#endif

#define O_CLOEXEC _O_NOINHERIT
#define O_NONBLOCK 0x40000000
#define FD_CLOEXEC 1
#define F_GETFL 1
#define F_SETFL 2
#define F_SETFD 3
#define F_DUPFD_CLOEXEC 4
#define F_SETPIPE_SZ 5
#define SOCK_NONBLOCK 0x10000000
#define SOCK_CLOEXEC 0x20000000
#define MSG_DONTWAIT 0x10000000
#define MSG_NOSIGNAL 0
#ifndef MSG_TRUNC
#define MSG_TRUNC 0x20000000
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#define WIFEXITED(status) 1
#define WEXITSTATUS(status) (status)
#define WTERMSIG(status) 0
#define WNOHANG 1
#define PR_SET_NAME 0
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)

struct linrad_pollfd { int fd; short events, revents; };
typedef unsigned long nfds_t;
struct sigaction { void (*sa_handler)(int); unsigned sa_mask; int sa_flags; };
int lw_sigaction(int sig, const struct sigaction *action, struct sigaction *old);
int lw_open(const char *path, int flags, ...);
int lw_close(int fd);
ssize_t lw_read(int fd, void *buffer, size_t size);
ssize_t lw_write(int fd, const void *buffer, size_t size);
int lw_fcntl(int fd, int command, ...);
int lw_pipe2(int fds[2], int flags);
int lw_poll(struct linrad_pollfd *fds, nfds_t count, int timeout);
int lw_socket(int family, int type, int protocol);
int lw_bind(int fd, const struct sockaddr *address, int length);
int lw_connect(int fd, const struct sockaddr *address, int length);
int lw_listen(int fd, int backlog);
int lw_accept(int fd, struct sockaddr *address, int *length);
int lw_setsockopt(int fd, int level, int option, const void *value, int length);
int lw_getsockopt(int fd, int level, int option, void *value, int *length);
int lw_getsockname(int fd, struct sockaddr *address, int *length);
ssize_t lw_send(int fd, const void *buffer, size_t size, int flags);
ssize_t lw_recv(int fd, void *buffer, size_t size, int flags);
ssize_t lw_sendto(int fd, const void *buffer, size_t size, int flags, const struct sockaddr *address, int length);
ssize_t lw_recvfrom(int fd, void *buffer, size_t size, int flags, struct sockaddr *address, int *length);
void *lw_mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset);
int lw_munmap(void *address, size_t size);
ssize_t lw_readlink(const char *path, char *buffer, size_t size);
char *lw_file_read_link(const char *path, GError **error);
void lw_stop_process(GPid process);
GPid lw_waitpid(GPid process, int *status, int options);
gboolean lw_open_pipe(int fds[2], int flags, GError **error);
gboolean lw_set_nonblocking(int fd, gboolean enabled, GError **error);
guint lw_fd_add(int fd, GIOCondition condition, gboolean (*callback)(int, GIOCondition, gpointer), gpointer data);
guint lw_signal_add(int signal_number, GSourceFunc callback, gpointer data);
gboolean lw_spawn_fds(const gchar *directory, gchar **argv, gchar **env, GSpawnFlags flags,
    GSpawnChildSetupFunc setup, gpointer data, GPid *pid, gint input, gint output, gint error_fd, GError **error);
gboolean lw_spawn_pipes(const gchar *directory, gchar **argv, gchar **env, GSpawnFlags flags,
    GSpawnChildSetupFunc setup, gpointer data, GPid *pid, gint *input, gint *output, gint *error_fd, GError **error);
void *lw_aligned_alloc(size_t alignment, size_t size);
void lw_aligned_free(void *pointer);
int lw_posix_memalign(void **pointer, size_t alignment, size_t size);

#ifdef __cplusplus
}
#endif
#ifndef LINRAD_WIN32_IMPLEMENTATION
#define open(...) lw_open(__VA_ARGS__)
#define close(fd) lw_close(fd)
#define read(...) lw_read(__VA_ARGS__)
#define write(...) lw_write(__VA_ARGS__)
#define fcntl(...) lw_fcntl(__VA_ARGS__)
#define pipe2(...) lw_pipe2(__VA_ARGS__)
#define pollfd linrad_pollfd
#define poll(...) lw_poll(__VA_ARGS__)
#define socket(...) lw_socket(__VA_ARGS__)
#define bind(...) lw_bind(__VA_ARGS__)
#define connect(...) lw_connect(__VA_ARGS__)
#define listen(...) lw_listen(__VA_ARGS__)
#define accept(...) lw_accept(__VA_ARGS__)
#define setsockopt(...) lw_setsockopt(__VA_ARGS__)
#define getsockopt(...) lw_getsockopt(__VA_ARGS__)
#define getsockname(...) lw_getsockname(__VA_ARGS__)
#define send(...) lw_send(__VA_ARGS__)
#define recv(...) lw_recv(__VA_ARGS__)
#define sendto(...) lw_sendto(__VA_ARGS__)
#define recvfrom(...) lw_recvfrom(__VA_ARGS__)
#define mmap(...) lw_mmap(__VA_ARGS__)
#define munmap(...) lw_munmap(__VA_ARGS__)
#define readlink(...) lw_readlink(__VA_ARGS__)
#define g_file_read_link(...) lw_file_read_link(__VA_ARGS__)
#define g_unix_open_pipe(...) lw_open_pipe(__VA_ARGS__)
#define g_unix_set_fd_nonblocking(...) lw_set_nonblocking(__VA_ARGS__)
#define g_unix_fd_add(...) lw_fd_add(__VA_ARGS__)
#define g_unix_signal_add(...) lw_signal_add(__VA_ARGS__)
#define g_spawn_async_with_fds(...) lw_spawn_fds(__VA_ARGS__)
#define g_spawn_async_with_pipes(...) lw_spawn_pipes(__VA_ARGS__)
#define sigaction(...) lw_sigaction(__VA_ARGS__)
#define sigemptyset(set) (*(set)=0)
#define setsid() 0
#define prctl(...) 0
#define waitpid(...) lw_waitpid(__VA_ARGS__)
#define setenv(key,value,overwrite) (g_setenv(key,value,overwrite) ? 0 : -1)
#define posix_memalign(...) lw_posix_memalign(__VA_ARGS__)
#define aligned_alloc(...) lw_aligned_alloc(__VA_ARGS__)
#endif
#endif
