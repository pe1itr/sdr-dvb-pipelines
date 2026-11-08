#define _POSIX_C_SOURCE 200809L
#include "iq_file.h"
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static GQuark file_error(void) { return g_quark_from_static_string("iq-file-error"); }
static gboolean invalid(GError **error, const char *message) {
    g_set_error_literal(error,file_error(),1,message); return FALSE;
}
void iq_file_info_clear(IQFileInfo *info) {
    g_free(info->path); g_free(info->metadata_path); memset(info,0,sizeof(*info));
}
static json_object *member(json_object *obj, const char *key) {
    json_object *value=NULL;
    if (obj && json_object_is_type(obj,json_type_object)) json_object_object_get_ex(obj,key,&value);
    return value;
}
static const char *string(json_object *obj, const char *key) {
    json_object *value=member(obj,key);
    return value && json_object_is_type(value,json_type_string) ? json_object_get_string(value) : NULL;
}
static gboolean number(json_object *obj, const char *key, double *out, GError **error) {
    json_object *value=member(obj,key);
    if (!value) return TRUE;
    if ((!json_object_is_type(value,json_type_int) && !json_object_is_type(value,json_type_double)) ||
        !isfinite(json_object_get_double(value))) {
        g_set_error(error,file_error(),1,"JSON field %s must be a finite number",key); return FALSE;
    }
    *out=json_object_get_double(value); return TRUE;
}
static gboolean integer(json_object *obj, const char *key, int low, int high, int *out, GError **error) {
    if (!member(obj,key)) return TRUE;
    double value=0;
    if (!number(obj,key,&value,error)) return FALSE;
    if (value<low || value>high || value!=floor(value)) {
        g_set_error(error,file_error(),1,"JSON field %s is outside %d..%d",key,low,high); return FALSE;
    }
    *out=(int)value; return TRUE;
}
static gboolean text_field(json_object *obj, const char *key, char *out, size_t size, GError **error) {
    if (!member(obj,key)) return TRUE;
    const char *value=string(obj,key);
    if (!value || !*value || strlen(value)>=size) {
        g_set_error(error,file_error(),1,"Invalid JSON field %s",key); return FALSE;
    }
    g_strlcpy(out,value,size); return TRUE;
}
static char *sidecar(const char *path) {
    char *candidate=g_strconcat(path,".json",NULL);
    if (g_file_test(candidate,G_FILE_TEST_EXISTS)) return candidate;
    g_free(candidate);
    char *copy=g_strdup(path), *dot=strrchr(copy,'.'), *slash=strrchr(copy,'/');
    if (dot && (!slash || dot>slash)) *dot=0;
    candidate=g_strconcat(copy,".json",NULL); g_free(copy);
    if (g_file_test(candidate,G_FILE_TEST_EXISTS)) return candidate;
    g_free(candidate); return NULL;
}
static json_object *read_json(const char *path, GError **error) {
    GStatBuf st;
    if (g_stat(path,&st) || !S_ISREG(st.st_mode) || st.st_size>1024*1024) {
        invalid(error,"JSON file is missing, is not a regular file or exceeds 1 MiB"); return NULL;
    }
    char *data=NULL; gsize length;
    if (!g_file_get_contents(path,&data,&length,error)) return NULL;
    struct json_tokener *tok=json_tokener_new_ex(32);
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
    json_object *obj=json_tokener_parse_ex(tok,data,(int)length);
#if JSON_C_VERSION_NUM >= 0x000e00
    size_t end=json_tokener_get_parse_end(tok);
#else
    /* json-c 0.13 on Ubuntu 20.04 exposes the offset in the public tokener. */
    size_t end=(size_t)tok->char_offset;
#endif
    while (end<length && g_ascii_isspace(data[end])) ++end;
    if (json_tokener_get_error(tok)!=json_tokener_success || end!=length ||
        !obj || !json_object_is_type(obj,json_type_object)) {
        if (obj) json_object_put(obj);
        obj=NULL; g_set_error(error,file_error(),1,"Invalid JSON object: %s",path);
    }
    json_tokener_free(tok); g_free(data); return obj;
}
static void filename_text(const char *name, const char *pattern, char *out, size_t size) {
    GRegex *re=g_regex_new(pattern,G_REGEX_CASELESS,0,NULL); GMatchInfo *match=NULL;
    if (g_regex_match(re,name,0,&match)) {
        char *value=g_match_info_fetch(match,1); g_strlcpy(out,value,size); g_free(value);
    }
    g_match_info_free(match); g_regex_unref(re);
}
static void from_filename(const char *path, IQFileInfo *info) {
    char *name=g_path_get_basename(path), value[32]={0};
    filename_text(name,"(?:^|[-_])(dvbs2|dvbs|dvbt2|dvbt)(?=[-_.])",value,sizeof(value));
    if (*value) g_strlcpy(info->standard,!g_ascii_strcasecmp(value,"dvbt2") ? "DVB-T2" : !g_ascii_strcasecmp(value,"dvbt") ? "DVB-T" :
        !g_ascii_strcasecmp(value,"dvbs2") ? "DVB-S2" : "DVB-S",sizeof(info->standard));
    value[0]=0; filename_text(name,"(?:^|[-_])(qpsk|8psk)(?=[-_.])",value,sizeof(value));
    if (*value) { char *upper=g_ascii_strup(value,-1); g_strlcpy(info->constellation,upper,sizeof(info->constellation)); g_free(upper); }
    filename_text(name,"(?:^|[-_])fec([0-9]+-[0-9]+)(?=[-_.])",info->fec,sizeof(info->fec));
    filename_text(name,"(?:^|[-_])gi([0-9]+-[0-9]+)(?=[-_.])",info->gi,sizeof(info->gi));
    for (char *p=info->fec; *p; ++p) if (*p=='-') *p='/';
    for (char *p=info->gi; *p; ++p) if (*p=='-') *p='/';
    value[0]=0; filename_text(name,"(?:^|[-_])(?:sr|bw)([0-9]{1,8})(?=[-_.])",value,sizeof(value));
    if (*value) info->symbol_rate=atoi(value);
    value[0]=0; filename_text(name,"(?:^|[-_])fs([0-9]{1,9})(?=[-_.])",value,sizeof(value));
    if (*value) info->sample_rate=atoi(value);
    if (g_str_has_suffix(name,".cf32") || g_str_has_suffix(name,".fc32")) g_strlcpy(info->format,"cf32",sizeof(info->format));
    else if (g_str_has_suffix(name,".s16") || g_str_has_suffix(name,"_s16.iq")) g_strlcpy(info->format,"s16",sizeof(info->format));
    else if (g_str_has_suffix(name,".u8") || g_str_has_suffix(name,"_u8.iq")) g_strlcpy(info->format,"u8",sizeof(info->format));
    g_free(name);
}
static gboolean dvb_metadata(json_object *obj, IQFileInfo *info, GError **error) {
    json_object *config=member(obj,"config");
    if (!text_field(obj,"standard",info->standard,sizeof(info->standard),error) ||
        !text_field(obj,"modulation",info->constellation,sizeof(info->constellation),error) ||
        !text_field(obj,"fec",info->fec,sizeof(info->fec),error) ||
        !text_field(obj,"gi",info->gi,sizeof(info->gi),error)) return FALSE;
    /* datv-transmitter's config uses its own numeric FEC enum. */
    if (member(config,"fec")) {
        int fec=0;
        if (!integer(config,"fec",0,10,&fec,error)) return FALSE;
        const char *label=fec==3 ? "1/2" : fec==4 ? "3/5" : fec==5 ? "2/3" :
            fec==6 ? "3/4" : fec==8 ? "5/6" : fec==10 ? "7/8" : NULL;
        if (!label) return invalid(error,"Unknown FEC code in transmitter metadata");
        g_strlcpy(info->fec,label,sizeof(info->fec));
    }
    if (!strcmp(info->standard,"DVB-T")) {
        if (!integer(config,"bandwidth",1,100000000,&info->symbol_rate,error) ||
            !integer(obj,"bandwidth_hz",1,100000000,&info->symbol_rate,error)) return FALSE;
        if (member(config,"guard")) {
            int guard=0;
            if (!integer(config,"guard",1,32,&guard,error)) return FALSE;
            g_snprintf(info->gi,sizeof(info->gi),"1/%d",guard);
        }
    } else if (!integer(config,"sr",1,100000000,&info->symbol_rate,error)) return FALSE;
    if (!integer(obj,"symbol_rate",1,100000000,&info->symbol_rate,error) ||
        !integer(obj,"symbol_rate_hz",1,100000000,&info->symbol_rate,error)) return FALSE;
    if (member(config,"frequency")) {
        if (!number(config,"frequency",&info->frequency,error)) return FALSE;
        info->has_frequency=TRUE;
    }
    return TRUE;
}
gboolean iq_file_read_info(const char *path, IQFileInfo *info, GError **error) {
    memset(info,0,sizeof(*info)); info->path=g_canonicalize_filename(path,NULL);
    GStatBuf st;
    if (g_stat(info->path,&st) || !S_ISREG(st.st_mode) || !st.st_size) {
        invalid(error,"Select an existing, non-empty IQ file"); goto fail;
    }
    from_filename(info->path,info);
    info->metadata_path=sidecar(info->path);
    if (!info->metadata_path) return TRUE;
    json_object *obj=read_json(info->metadata_path,error);
    if (!obj) goto fail;
    json_object *params=member(obj,"parameters");
    const char *original=string(params,"iq_in");
    if (original && *original) {
        char *dir=g_path_get_dirname(info->path), *origin=g_canonicalize_filename(original,dir), *origin_json=sidecar(origin);
        g_free(dir); g_free(origin);
        if (origin_json && strcmp(origin_json,info->metadata_path)) {
            json_object *origin_obj=read_json(origin_json,error);
            if (!origin_obj || !dvb_metadata(origin_obj,info,error)) {
                if (origin_obj) json_object_put(origin_obj);
                g_free(origin_json); json_object_put(obj); goto fail;
            }
            json_object_put(origin_obj);
        }
        g_free(origin_json);
    }
    gboolean ok=dvb_metadata(obj,info,error) &&
        integer(obj,"sample_rate",1,100000000,&info->sample_rate,error) &&
        integer(obj,"sample_rate_hz",1,100000000,&info->sample_rate,error) &&
        integer(obj,"header_bytes",0,100000000,&info->header_bytes,error);
    const char *format=string(obj,"format");
    if (ok && member(obj,"format")) {
        const char *normalized=NULL;
        if (format && (!strcmp(format,"s16") || !strcmp(format,"cs16_le"))) normalized="s16";
        if (format && (!strcmp(format,"u8") || !strcmp(format,"cu8"))) normalized="u8";
        if (format && (!strcmp(format,"cf32") || !strcmp(format,"fc32") || !strcmp(format,"cf32_le"))) normalized="cf32";
        if (!normalized) ok=invalid(error,"Unsupported IQ format (s16, u8 or cf32 required)");
        else g_strlcpy(info->format,normalized,sizeof(info->format));
    }
    const char *order=string(obj,"byte_order"), *layout=string(obj,"layout");
    if (ok && ((member(obj,"byte_order") && (!order || strcmp(order,"little"))) ||
               (member(obj,"layout") && (!layout || strcmp(layout,"IQ")))))
        ok=invalid(error,"Only little-endian IQ ordering is supported");
    if (ok && member(params,"frequency_mhz")) {
        ok=number(params,"frequency_mhz",&info->frequency,error);
        info->frequency*=1e6; info->has_frequency=TRUE;
    }
    if (ok && member(obj,"center_frequency_hz")) { ok=number(obj,"center_frequency_hz",&info->frequency,error); info->has_frequency=TRUE; }
    if (ok && (info->frequency<0 || info->frequency>1e12)) ok=invalid(error,"Invalid receive frequency in metadata");
    json_object_put(obj);
    if (ok) return TRUE;
fail:
    iq_file_info_clear(info); return FALSE;
}
gboolean iq_file_validate(const char *path, const char *format, int header_bytes, guint64 *samples, GError **error) {
    int stride=!strcmp(format,"s16") ? 4 : !strcmp(format,"u8") ? 2 : !strcmp(format,"cf32") ? 8 : 0;
    if (!stride || header_bytes<0) return invalid(error,"Invalid IQ format or header length");
    GStatBuf st;
    if (g_stat(path,&st) || !S_ISREG(st.st_mode)) return invalid(error,"IQ file not found or not a regular file");
    if (st.st_size<=header_bytes || (st.st_size-header_bytes)%stride)
        return invalid(error,"IQ file is empty or contains an incomplete I/Q sample");
    FILE *file=g_fopen(path,"rb");
    if (!file) { g_set_error(error,file_error(),errno,"Cannot read IQ file: %s",g_strerror(errno)); return FALSE; }
    fclose(file);
    *samples=(st.st_size-header_bytes)/stride; return TRUE;
}
