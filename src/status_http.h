#ifndef STATUS_HTTP_H
#define STATUS_HTTP_H
#include <gio/gio.h>
typedef struct StatusHttp StatusHttp;
/* Called on the main context. Return an allocated UTF-8 JSON document. */
typedef char *(*StatusHttpSnapshot)(gpointer data);
/* Return an HTTP status line fragment and set a borrowed error message on failure. */
typedef const char *(*StatusHttpControl)(gpointer data, const char *command, const char **error);
StatusHttp *status_http_start(const char *address, guint16 port,
    StatusHttpSnapshot snapshot, StatusHttpSnapshot constellation, StatusHttpControl control, gpointer data, GError **error);
guint16 status_http_port(StatusHttp *server);
char *status_http_network_info(StatusHttp *server);
void status_http_stop(StatusHttp *server);
#endif
