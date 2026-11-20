#ifndef LINRAD_DSMCC_OUTPUT_H
#define LINRAD_DSMCC_OUTPUT_H
#include <glib.h>

typedef struct DsmccOutput DsmccOutput;
DsmccOutput *dsmcc_output_open(const char *host, int port, GError **error);
void dsmcc_output_feed(DsmccOutput *out, const guint8 *bytes, gsize size);
void dsmcc_output_reset(DsmccOutput *out);
void dsmcc_output_free(DsmccOutput *out);
#endif
