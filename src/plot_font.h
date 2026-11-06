/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LINRAD_PLOT_FONT_H
#define LINRAD_PLOT_FONT_H
#include <gtk/gtk.h>

/* Use the same font backend for plotted labels and GTK controls. Cairo's toy
 * Win32 font mapper does not use Pango's fontconfig selection. */
static inline void plot_font(cairo_t *cr,const char *family,cairo_font_slant_t slant,cairo_font_weight_t weight) {
#ifdef _WIN32
    PangoFontMap *map=pango_cairo_font_map_get_default();
    PangoContext *context=pango_font_map_create_context(map);
    PangoFontDescription *description=pango_font_description_new();
    pango_font_description_set_family(description,!strcmp(family,"monospace") ? "DejaVu Sans Mono" : "DejaVu Sans");
    pango_font_description_set_size(description,12*PANGO_SCALE);
    pango_font_description_set_style(description,slant==CAIRO_FONT_SLANT_NORMAL ? PANGO_STYLE_NORMAL : PANGO_STYLE_ITALIC);
    pango_font_description_set_weight(description,weight==CAIRO_FONT_WEIGHT_BOLD ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    PangoFont *font=pango_font_map_load_font(map,context,description);
    if (font) {
        cairo_scaled_font_t *scaled=pango_cairo_font_get_scaled_font(PANGO_CAIRO_FONT(font));
        cairo_set_font_face(cr,cairo_scaled_font_get_font_face(scaled));
        g_object_unref(font);
    }
    pango_font_description_free(description); g_object_unref(context);
#else
    cairo_select_font_face(cr,family,slant,weight);
#endif
}
#endif
