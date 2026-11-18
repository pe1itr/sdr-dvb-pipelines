#ifndef STATUS_HTTP_H
#define STATUS_HTTP_H
#include <gio/gio.h>
typedef struct StatusHttp StatusHttp;
/* Called on the main context. Return an allocated UTF-8 JSON document. */
typedef char *(*StatusHttpSnapshot)(gpointer data);
StatusHttp *status_http_start(const char *address, guint16 port,
    StatusHttpSnapshot snapshot, StatusHttpSnapshot constellation, gpointer data, GError **error);
guint16 status_http_port(StatusHttp *server);
void status_http_stop(StatusHttp *server);
#endif
