#include "status_http.h"
#include <string.h>

/* All callbacks run on the main context; no network wait blocks reception.
 * Bound request size, connections and total lifetime, including slow readers. */
struct StatusHttp {
    GSocketService *service;
    GList *clients;
    StatusHttpSnapshot snapshot, constellation;
    gpointer data;
    guint16 port;
};
typedef struct {
    StatusHttp *server;
    GSocketConnection *connection;
    GCancellable *cancel;
    guint timer;
    char request[8193];
    gsize used;
    char *response;
} Client;

static void client_free(Client *c) {
    if (c->server) c->server->clients=g_list_remove(c->server->clients,c);
    if (c->timer) g_source_remove(c->timer);
    g_io_stream_close(G_IO_STREAM(c->connection),NULL,NULL);
    g_object_unref(c->connection);g_object_unref(c->cancel);
    g_free(c->response);g_free(c);
}
static gboolean expired(gpointer data) {
    Client *c=data;c->timer=0;g_cancellable_cancel(c->cancel);
    return G_SOURCE_REMOVE;
}
static void written(GObject *object,GAsyncResult *result,gpointer data) {
    g_output_stream_write_all_finish(G_OUTPUT_STREAM(object),result,NULL,NULL);
    client_free(data);
}
static void respond(Client *c,const char *code,const char *body,const char *extra) {
    c->response=g_strdup_printf("HTTP/1.1 %s\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Content-Length: %" G_GSIZE_FORMAT "\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nConnection: close\r\n%s\r\n%s",code,strlen(body),extra,body);
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(c->connection)),
        c->response,strlen(c->response),G_PRIORITY_DEFAULT,c->cancel,written,c);
}
static void read_request(Client *c);
static void received(GObject *object,GAsyncResult *result,gpointer data) {
    Client *c=data;
    gssize n=g_input_stream_read_finish(G_INPUT_STREAM(object),result,NULL);
    if (n<=0 || !c->server) {client_free(c);return;}
    if (memchr(c->request+c->used,0,n)) {respond(c,"400 Bad Request","{\"error\":\"invalid request\"}","");return;}
    c->used+=n;c->request[c->used]=0;
    if (!strstr(c->request,"\r\n\r\n")) {
        if (c->used==sizeof(c->request)-1) respond(c,"431 Request Header Fields Too Large","{\"error\":\"request too large\"}","");
        else read_request(c);
        return;
    }
    char *end=strstr(c->request,"\r\n");*end=0;
    char **parts=g_strsplit(c->request," ",0);
    if (g_strv_length(parts)!=3 || (strcmp(parts[2],"HTTP/1.0") && strcmp(parts[2],"HTTP/1.1")))
        respond(c,"400 Bad Request","{\"error\":\"invalid request\"}","");
    else if (strcmp(parts[0],"GET")) respond(c,"405 Method Not Allowed","{\"error\":\"GET required\"}","Allow: GET\r\n");
    else if (strcmp(parts[1],"/status") && strcmp(parts[1],"/constellation")) respond(c,"404 Not Found","{\"error\":\"unknown route\"}","");
    else {
        StatusHttpSnapshot snapshot = !strcmp(parts[1],"/constellation") ? c->server->constellation : c->server->snapshot;
        char *json=snapshot(c->server->data);
        respond(c,"200 OK",json,"");g_free(json);
    }
    g_strfreev(parts);
}
static void read_request(Client *c) {
    g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(c->connection)),
        c->request+c->used,sizeof(c->request)-1-c->used,G_PRIORITY_DEFAULT,c->cancel,received,c);
}
static gboolean incoming(GSocketService *service,GSocketConnection *connection,GObject *source,gpointer data) {
    (void)service;(void)source;StatusHttp *s=data;
    if (g_list_length(s->clients)>=16) {g_io_stream_close(G_IO_STREAM(connection),NULL,NULL);return TRUE;}
    Client *c=g_new0(Client,1);c->server=s;c->connection=g_object_ref(connection);c->cancel=g_cancellable_new();
    s->clients=g_list_prepend(s->clients,c);c->timer=g_timeout_add_seconds(3,expired,c);
    read_request(c);return TRUE;
}
StatusHttp *status_http_start(const char *address,guint16 port,StatusHttpSnapshot snapshot,StatusHttpSnapshot constellation,gpointer data,GError **error) {
    GInetAddress *ip=g_inet_address_new_from_string(address);
    if (!ip) {g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"Status bind address must be a numeric IPv4 or IPv6 address");return NULL;}
    StatusHttp *s=g_new0(StatusHttp,1);s->service=g_socket_service_new();s->snapshot=snapshot;s->constellation=constellation;s->data=data;
    GSocketAddress *bind=g_inet_socket_address_new(ip,port), *effective=NULL;g_object_unref(ip);
    gboolean ok=g_socket_listener_add_address(G_SOCKET_LISTENER(s->service),bind,G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP,NULL,&effective,error);g_object_unref(bind);
    if (!ok) {status_http_stop(s);return NULL;}
    s->port=g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));g_object_unref(effective);
    g_signal_connect(s->service,"incoming",G_CALLBACK(incoming),s);g_socket_service_start(s->service);return s;
}
guint16 status_http_port(StatusHttp *s) {return s->port;}
void status_http_stop(StatusHttp *s) {
    if (!s) return;
    g_socket_service_stop(s->service);g_socket_listener_close(G_SOCKET_LISTENER(s->service));
    g_signal_handlers_disconnect_by_data(s->service,s);
    for (GList *l=s->clients;l;l=l->next) {Client *c=l->data;c->server=NULL;g_cancellable_cancel(c->cancel);}
    g_list_free(s->clients);g_object_unref(s->service);g_free(s);
}
