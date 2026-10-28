#ifndef IQ_FILE_H
#define IQ_FILE_H
#include <glib.h>

typedef struct {
    char *path, *metadata_path;
    char format[8], standard[16], constellation[16], fec[16], gi[16];
    int sample_rate, header_bytes, symbol_rate;
    double frequency;
    gboolean has_frequency;
} IQFileInfo;

/* Read the selected file's format/rate and DVB metadata. Missing fields stay
 * empty/zero for manual entry. Malformed sidecars are reported, never guessed. */
gboolean iq_file_read_info(const char *path, IQFileInfo *info, GError **error);
void iq_file_info_clear(IQFileInfo *info);
gboolean iq_file_validate(const char *path, const char *format, int header_bytes,
                          guint64 *samples, GError **error);
#endif
