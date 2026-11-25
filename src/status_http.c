#define _DEFAULT_SOURCE
#include "status_http.h"
#include "api_docs.h"
#ifdef G_OS_WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif
#include <string.h>
#include <json-c/json.h>

/* All callbacks run on the main context; no network wait blocks reception.
 * Bound request size, connections and total lifetime, including slow readers. */
struct StatusHttp {
    GSocketService *service;
    GList *clients;
    StatusHttpSnapshot snapshot, constellation;
    StatusHttpControl control;
    DsmccMessages *messages;
    gpointer data;
    GInetAddress *bind_ip;
    gboolean accepts_ipv4;
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
static void respond_as(Client *c,const char *code,const char *mime,const char *body,const char *extra) {
    c->response=g_strdup_printf("HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\n"
        "Content-Length: %" G_GSIZE_FORMAT "\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nConnection: close\r\n%s\r\n%s",code,mime,strlen(body),extra,body);
    g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(c->connection)),
        c->response,strlen(c->response),G_PRIORITY_DEFAULT,c->cancel,written,c);
}
static void respond(Client *c,const char *code,const char *body,const char *extra) {
    respond_as(c,code,"application/json",body,extra);
}
static void read_request(Client *c);
/* Control requests encode all arguments in the path and take no body. */
static gboolean empty_body(const char *headers) {
    char **lines=g_strsplit(headers,"\r\n",-1);
    gboolean empty=TRUE;
    for (int i=0;lines[i][0];++i) {
        char *value=strchr(lines[i],':');
        if (!value) {empty=FALSE;break;}
        *value++=0;
        if (!g_ascii_strcasecmp(lines[i],"Transfer-Encoding") ||
            (!g_ascii_strcasecmp(lines[i],"Content-Length") && strcmp(g_strstrip(value),"0"))) {
            empty=FALSE;break;
        }
    }
    g_strfreev(lines);
    return empty;
}
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
    else if (g_str_has_prefix(parts[1],"/control/")) {
        if (strcmp(parts[0],"PUT")) respond(c,"405 Method Not Allowed","{\"error\":\"PUT required\"}","Allow: PUT\r\n");
        else if (!c->server->control) respond(c,"403 Forbidden","{\"error\":\"control unavailable\"}","");
        else if (!empty_body(end+2)) respond(c,"400 Bad Request","{\"error\":\"empty body required\"}","");
        else {
            const char *error=NULL;
            const char *code=c->server->control(c->server->data,parts[1]+strlen("/control/"),&error);
            json_object *reply=json_object_new_object();
            if (error) json_object_object_add(reply,"error",json_object_new_string(error));
            else json_object_object_add(reply,"accepted",json_object_new_boolean(TRUE));
            respond(c,code,json_object_to_json_string_ext(reply,JSON_C_TO_STRING_PLAIN),"");
            json_object_put(reply);
        }
    }
    else if (strcmp(parts[0],"GET")) respond(c,"405 Method Not Allowed","{\"error\":\"GET required\"}","Allow: GET\r\n");
    else if (!strcmp(parts[1],"/docs"))
        respond_as(c,"200 OK","text/html",api_reference_html,"");
    else if (!strcmp(parts[1],"/openapi.json")) respond(c,"200 OK",api_openapi_json,"");
    else if (!strcmp(parts[1],"/dsmcc/messages") || g_str_has_prefix(parts[1],"/dsmcc/messages?")) {
        const char *query=strchr(parts[1],'?');
        guint64 cursor=0;
        if (query && (!g_str_has_prefix(query,"?after=") ||
            !g_ascii_isdigit(query[7]) ||
            !g_ascii_string_to_unsigned(query+7,10,0,G_MAXINT64,&cursor,NULL))) {
            respond(c,"400 Bad Request","{\"error\":\"expected a nonnegative after cursor\"}","");
        } else {
            json_object *reply=dsmcc_messages_read(c->server->messages,query ? (gint64)cursor : -1);
            respond(c,"200 OK",json_object_to_json_string_ext(reply,JSON_C_TO_STRING_PLAIN),"");
            json_object_put(reply);
        }
    }
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
static void listener_event(GSocketListener *listener, GSocketListenerEvent event,
                           GSocket *socket, gpointer data) {
    (void)listener;
    if (event == G_SOCKET_LISTENER_LISTENED)
        ((StatusHttp *)data)->accepts_ipv4 = g_socket_speaks_ipv4(socket);
}

static void append_interface(GString *text, StatusHttp *s, const char *name,
                             struct sockaddr *address, gsize size) {
    if (!address || (address->sa_family != AF_INET && address->sa_family != AF_INET6)) return;
    GSocketAddress *socket_address = g_socket_address_new_from_native(address, size);
    if (!socket_address) return;
    GInetAddress *ip = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(socket_address));
    gboolean wildcard = g_inet_address_get_is_any(s->bind_ip);
    gboolean ipv4 = g_inet_address_get_family(ip) == G_SOCKET_FAMILY_IPV4;
    gboolean matches = wildcard
        ? (ipv4 ? s->accepts_ipv4 : g_inet_address_get_family(s->bind_ip) == G_SOCKET_FAMILY_IPV6)
        : g_inet_address_equal(s->bind_ip, ip);
    if (matches && !g_inet_address_get_is_any(ip) && !(wildcard && g_inet_address_get_is_loopback(ip))) {
        char *literal = g_inet_address_to_string(ip);
        if (text->len) g_string_append_c(text, '\n');
        g_string_append_printf(text, "%s   %s%s%s:%u", name, ipv4 ? "" : "[", literal,
                               ipv4 ? "" : "]", s->port);
        if (g_inet_address_get_is_loopback(ip)) g_string_append(text, " (this computer only)");
        if (!ipv4 && g_inet_address_get_is_link_local(ip)) g_string_append(text, " (link-local)");
        g_free(literal);
    }
    g_object_unref(socket_address);
}

char *status_http_network_info(StatusHttp *s) {
    if (!s) return g_strdup("Status endpoint is disabled");
    GString *text = g_string_new(NULL);
#ifdef G_OS_WIN32
    ULONG size = 15000, result;
    IP_ADAPTER_ADDRESSES *adapters = NULL;
    do {
        adapters = g_realloc(adapters, size);
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER, NULL, adapters, &size);
    } while (result == ERROR_BUFFER_OVERFLOW);
    if (result == NO_ERROR) {
        for (IP_ADAPTER_ADDRESSES *a = adapters; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            char *name = g_utf16_to_utf8((const gunichar2 *)a->FriendlyName, -1, NULL, NULL, NULL);
            for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
                append_interface(text, s, name ? name : a->AdapterName, u->Address.lpSockaddr,
                                 u->Address.iSockaddrLength);
            g_free(name);
        }
    }
    g_free(adapters);
#else
    struct ifaddrs *interfaces;
    if (getifaddrs(&interfaces) == 0) {
        for (struct ifaddrs *a = interfaces; a; a = a->ifa_next) {
            if (!(a->ifa_flags & IFF_UP) || !a->ifa_addr) continue;
            char *name = g_utf8_make_valid(a->ifa_name, -1);
            append_interface(text, s, name, a->ifa_addr, a->ifa_addr->sa_family == AF_INET
                             ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
            g_free(name);
        }
        freeifaddrs(interfaces);
    }
#endif
    if (!text->len) g_string_append(text, "No active network address found for this endpoint");
    return g_string_free(text, FALSE);
}

StatusHttp *status_http_start(const char *address,guint16 port,StatusHttpSnapshot snapshot,StatusHttpSnapshot constellation,StatusHttpControl control,DsmccMessages *messages,gpointer data,GError **error) {
    GInetAddress *ip=g_inet_address_new_from_string(address);
    if (!ip) {g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_INVALID_ARGUMENT,"Status bind address must be a numeric IPv4 or IPv6 address");return NULL;}
    StatusHttp *s=g_new0(StatusHttp,1);s->service=g_socket_service_new();s->snapshot=snapshot;s->constellation=constellation;s->data=data;s->control=control;s->messages=messages;
    GSocketAddress *bind=g_inet_socket_address_new(ip,port), *effective=NULL;
    s->bind_ip=ip;
    g_signal_connect(s->service,"event",G_CALLBACK(listener_event),s);
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
    g_clear_object(&s->bind_ip);
    g_list_free(s->clients);g_object_unref(s->service);g_free(s);
}
