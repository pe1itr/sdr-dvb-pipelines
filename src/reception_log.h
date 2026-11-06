#ifndef RECEPTION_LOG_H
#define RECEPTION_LOG_H
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <gio/gio.h>
#include "maidenhead.h"

enum { LOG_TIME, LOG_FREQUENCY, LOG_SERVICE, LOG_PROVIDER, LOG_MODE, LOG_TII, LOG_SOURCE,
       LOG_SYMBOL_RATE, LOG_BANDWIDTH, LOG_FEC, LOG_ATV_POINTS, LOG_ATV_DIRECTION, LOG_MY_CALLSIGN, LOG_MY_LOCATOR, LOG_COLUMNS };
static const char reception_log_legacy_header[]="timestamp_utc,frequency_hz,service_name,provider_name,mode,tii,source,symbol_rate_baud,bandwidth_hz,fec,atv_points,atv_direction\r\n";
static const char reception_log_header[]="timestamp_utc,frequency_hz,service_name,provider_name,mode,tii,source,symbol_rate_baud,bandwidth_hz,fec,atv_points,atv_direction,my_callsign,my_locator\r\n";

static gboolean reception_atv_valid(const char *points, const char *direction, GError **error) {
    gboolean valid=!*direction || !strcmp(direction,"ONE-WAY") || !strcmp(direction,"TWO-WAY");
    if (*points) {
        gboolean decimal=FALSE;
        valid=valid && g_ascii_isdigit(*points) && g_ascii_isdigit(points[strlen(points)-1]);
        for (const char *p=points;*p;++p) {
            if (*p=='.' && !decimal) decimal=TRUE;
            else if (!g_ascii_isdigit(*p)) valid=FALSE;
        }
        valid=valid && isfinite(g_ascii_strtod(points,NULL));
    }
    if (!valid) g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,
        "ATV points must be a non-negative number (decimal point), and direction ONE-WAY or TWO-WAY. Both may be left empty.");
    return valid;
}

/* Mode-independent CSV format, also usable by future digital decoders. */
static void reception_log_field(GString *row, const char *value) {
    g_string_append_c(row, '"');
    for (const char *p=value; *p; ++p) {
        if (*p=='"') g_string_append_c(row, '"');
        g_string_append_c(row, *p);
    }
    g_string_append_c(row, '"');
}

/* Read our CSV including quoted commas, doubled quotes and embedded newlines.
 * Refuse malformed files instead of silently omitting reception records. */
static GPtrArray *reception_log_load(const char *path, char **etag, GError **error) {
    GPtrArray *rows=g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
    char *contents=NULL; gsize size=0;
    GError *read_error=NULL;
    GFile *file=g_file_new_for_path(path);
    gboolean loaded=g_file_load_contents(file,NULL,&contents,&size,etag,&read_error);
    g_object_unref(file);
    if (!loaded) {
        if (g_error_matches(read_error,G_IO_ERROR,G_IO_ERROR_NOT_FOUND)) {
            g_error_free(read_error); return rows;
        }
        g_propagate_error(error,read_error); g_ptr_array_unref(rows); return NULL;
    }
    if (!size) { g_free(contents); return rows; }
    gboolean legacy=g_str_has_prefix(contents,reception_log_legacy_header);
    const char *header=legacy ? reception_log_legacy_header : reception_log_header;
    unsigned columns=legacy ? LOG_MY_CALLSIGN : LOG_COLUMNS;
    if (strlen(contents)!=size || !g_utf8_validate(contents,size,NULL) ||
        !g_str_has_prefix(contents,header)) goto invalid;
    const char *p=contents+strlen(header);
    while (*p) {
        char **row=g_new0(char *,LOG_COLUMNS+1);
        g_ptr_array_add(rows,row);
        for (unsigned i=0;i<columns;++i) {
            GString *value=g_string_new(NULL);
            if (*p=='"') {
                ++p;
                while (*p) {
                    if (*p=='"') {
                        ++p;
                        if (*p!='"') break;
                    }
                    g_string_append_c(value,*p++);
                }
            } else {
                while (*p && *p!=',' && *p!='\r' && *p!='\n' && *p!='"')
                    g_string_append_c(value,*p++);
            }
            row[i]=g_string_free(value,FALSE);
            if (i+1<columns) { if (*p++!=',') goto invalid; }
            else {
                if (*p=='\r') ++p;
                if (*p++!='\n') goto invalid;
            }
        }
        for (unsigned i=columns;i<LOG_COLUMNS;++i) row[i]=g_strdup("");
    }
    g_free(contents); return rows;
invalid:
    g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Invalid reception CSV: %s",path);
    g_free(contents); g_ptr_array_unref(rows); return NULL;
}

static GPtrArray *reception_log_read(const char *path, GError **error) {
    return reception_log_load(path,NULL,error);
}

/* Replace atomically, including upgrading old logs without losing their rows. */
static gboolean reception_log_write(const char *path, GPtrArray *rows, const char *etag, GError **error) {
    GString *contents=g_string_new(reception_log_header);
    for (unsigned i=0;i<rows->len;++i) {
        char **row=g_ptr_array_index(rows,i);
        for (unsigned j=0;j<LOG_COLUMNS;++j) {
            reception_log_field(contents,row[j]);
            g_string_append(contents,j+1==LOG_COLUMNS ? "\r\n" : ",");
        }
    }
    GFile *file=g_file_new_for_path(path);
    gboolean ok=g_file_replace_contents(file,contents->str,contents->len,etag,FALSE,
        G_FILE_CREATE_PRIVATE,NULL,NULL,error);
    g_object_unref(file); g_string_free(contents,TRUE);
    return ok;
}

static gboolean reception_station_valid(const char *callsign, const char *locator, GError **error) {
    for (const char *p=callsign;*p;++p) {
        if (!g_ascii_isalnum(*p) && *p!='/' && *p!='-') {
            g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,
                "My Callsign may contain letters, digits, / and -, or be empty.");
            return FALSE;
        }
    }
    gboolean valid=!*locator || maidenhead_center(locator,NULL,NULL);
    if (!valid) g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,
        "My Locator must be a 2, 4, 6 or 8 character Maidenhead locator (for example JO21QK), or empty.");
    return valid;
}

static gboolean reception_log_append(const char *path, const char *timestamp,
        double frequency, const char *service, const char *provider,
        const char *mode, const char *tii, const char *source,
        const char *symbol_rate, const char *bandwidth, const char *fec,
        const char *points, const char *direction, const char *callsign, const char *locator, GError **error) {
    if (!reception_atv_valid(points,direction,error) || !reception_station_valid(callsign,locator,error)) return FALSE;
    char *etag=NULL;
    GPtrArray *rows=reception_log_load(path,&etag,error);
    if (!rows) { g_free(etag); return FALSE; }
    char hz[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(hz,sizeof(hz),"%.0f",frequency);
    const char *row[]={timestamp,hz,service,provider,mode,tii,source,symbol_rate,bandwidth,fec,
        points,direction,callsign,locator,NULL};
    g_ptr_array_add(rows,g_strdupv((char **)row));
    gboolean ok=reception_log_write(path,rows,etag,error);
    g_ptr_array_unref(rows); g_free(etag);
    return ok;
}

static gboolean reception_log_replace(const char *path, unsigned index, char **original,
        char **replacement, GError **error) {
    if (!reception_atv_valid(replacement[LOG_ATV_POINTS],replacement[LOG_ATV_DIRECTION],error) ||
        !reception_station_valid(replacement[LOG_MY_CALLSIGN],replacement[LOG_MY_LOCATOR],error)) return FALSE;
    char *etag=NULL;
    GPtrArray *rows=reception_log_load(path,&etag,error);
    if (!rows) { g_free(etag); return FALSE; }
    gboolean same=index<rows->len;
    if (same) {
        char **current=g_ptr_array_index(rows,index);
        for (int j=0;j<LOG_COLUMNS;++j) if (strcmp(current[j],original[j])) same=FALSE;
    }
    if (!same) {
        g_set_error_literal(error,G_IO_ERROR,G_IO_ERROR_WRONG_ETAG,
            "This reception has changed on disk. Cancel, refresh the logbook and reopen the entry.");
        g_ptr_array_unref(rows); g_free(etag); return FALSE;
    }
    g_strfreev(g_ptr_array_index(rows,index));
    g_ptr_array_index(rows,index)=g_strdupv(replacement);
    gboolean ok=reception_log_write(path,rows,etag,error);
    g_ptr_array_unref(rows); g_free(etag);
    return ok;
}

static void reception_adif_field(GString *out, const char *name, const char *value) {
    if (*value) g_string_append_printf(out,"<%s:%zu>%s",name,strlen(value),value);
}

/* ADI uses ASCII. Keep readable names and a lossless UTF-8/base64 copy when
 * transliteration or control-character normalization changes an APP field. */
static void reception_adif_app(GString *out, const char *field, const char *value) {
    char *name=g_strconcat("APP_LINRADDVB_",field,NULL);
    char *ascii=g_str_to_ascii(value,"C");
    for (char *p=ascii;*p;++p) if ((guchar)*p<32 || (guchar)*p>126) *p=' ';
    reception_adif_field(out,name,ascii);
    if (strcmp(ascii,value)) {
        char *encoded=g_base64_encode((const guchar *)value,strlen(value));
        char *encoded_name=g_strconcat(name,"_UTF8_B64",NULL);
        reception_adif_field(out,encoded_name,encoded);
        g_free(encoded_name); g_free(encoded);
    }
    g_free(ascii); g_free(name);
}

static char *reception_log_adif(GPtrArray *rows, GError **error) {
    GString *out=g_string_new("Linrad DVB reception log\r\n<ADIF_VER:5>3.1.8<PROGRAMID:9>LINRADDVB\r\n<EOH>\r\n");
    for (unsigned i=0;i<rows->len;++i) {
        char **row=g_ptr_array_index(rows,i), *end=NULL;
        if (!reception_atv_valid(row[LOG_ATV_POINTS],row[LOG_ATV_DIRECTION],error) ||
            !reception_station_valid(row[LOG_MY_CALLSIGN],row[LOG_MY_LOCATOR],error)) {
            g_string_free(out,TRUE); return NULL;
        }
        GDateTime *time=g_date_time_new_from_iso8601(row[LOG_TIME],NULL);
        double frequency=g_ascii_strtod(row[LOG_FREQUENCY],&end);
        if (!time || end==row[LOG_FREQUENCY] || *end || !isfinite(frequency) || frequency<=0) {
            if (time) g_date_time_unref(time);
            g_set_error(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,
                "Cannot export row %u: valid UTC time and a frequency greater than zero are required.",i+1);
            g_string_free(out,TRUE); return NULL;
        }
        GDateTime *utc=g_date_time_to_utc(time);
        char *date=g_date_time_format(utc,"%Y%m%d"), *clock=g_date_time_format(utc,"%H%M%S");
        char mhz[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(mhz,sizeof(mhz),"%.6f",frequency/1e6);
        reception_adif_field(out,"QSO_DATE",date);
        reception_adif_field(out,"TIME_ON",clock);
        reception_adif_field(out,"FREQ",mhz);
        reception_adif_field(out,"FREQ_RX",mhz);
        reception_adif_field(out,"SWL","Y");
        reception_adif_field(out,"STATION_CALLSIGN",row[LOG_MY_CALLSIGN]);
        reception_adif_field(out,"MY_GRIDSQUARE",row[LOG_MY_LOCATOR]);
        /* The exact digital standard is retained separately. Never invent
         * a callsign from a broadcast service name or an unknown submode. */
        if (!strcmp(row[LOG_MODE],"DVB-S") || !strcmp(row[LOG_MODE],"DVB-S2") ||
            !strcmp(row[LOG_MODE],"DVB-T") || !strcmp(row[LOG_MODE],"DVB-T2"))
            reception_adif_field(out,"MODE","ATV");
        const char *names[]={"TIMESTAMP_UTC","FREQUENCY_HZ","SERVICE","PROVIDER","MODE","TII","SOURCE"};
        for (unsigned j=0;j<G_N_ELEMENTS(names);++j) reception_adif_app(out,names[j],row[j]);
        reception_adif_field(out,"APP_VERON_ATV_POINTS",row[LOG_ATV_POINTS]);
        reception_adif_field(out,"APP_VERON_ATV_DIRECTION",row[LOG_ATV_DIRECTION]);
        char *comment=g_strdup_printf("Service: %s; Provider: %s; Mode: %s; TII: %s; Source: %s; SR (baud): %s; BW (Hz): %s; FEC: %s",
            row[LOG_SERVICE],row[LOG_PROVIDER],row[LOG_MODE],row[LOG_TII],row[LOG_SOURCE],
            row[LOG_SYMBOL_RATE],row[LOG_BANDWIDTH],row[LOG_FEC]);
        char *ascii=g_str_to_ascii(comment,"C");
        for (char *p=ascii;*p;++p) if ((guchar)*p<32 || (guchar)*p>126) *p=' ';
        reception_adif_field(out,"COMMENT",ascii);
        g_string_append(out,"<EOR>\r\n");
        g_free(ascii); g_free(comment); g_free(date); g_free(clock);
        g_date_time_unref(utc); g_date_time_unref(time);
    }
    return g_string_free(out,FALSE);
}

static gboolean reception_log_export(const char *path, const char *log_path, GPtrArray *rows, GError **error) {
    char *target=g_canonicalize_filename(path,NULL), *source=g_canonicalize_filename(log_path,NULL);
    GStatBuf target_stat, source_stat;
    gboolean same=!strcmp(target,source) ||
        (!g_stat(target,&target_stat) && !g_stat(source,&source_stat) &&
         target_stat.st_dev==source_stat.st_dev && target_stat.st_ino==source_stat.st_ino);
    g_free(target); g_free(source);
    if (same) {
        g_set_error_literal(error,G_FILE_ERROR,G_FILE_ERROR_INVAL,"Choose an ADIF export file separate from the reception log.");
        return FALSE;
    }
    char *contents=reception_log_adif(rows,error);
    if (!contents) return FALSE;
    GFile *file=g_file_new_for_path(path);
    gboolean ok=g_file_replace_contents(file,contents,strlen(contents),NULL,FALSE,
        G_FILE_CREATE_PRIVATE,NULL,NULL,error);
    g_object_unref(file); g_free(contents); return ok;
}
#endif
