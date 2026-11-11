/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * Display device page for Wayland sessions, built from the kernel's KMS
 * state (works with any compositor and any GPU vendor).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses>.
 */

#ifndef __CTK_WL_DISPLAY_H__
#define __CTK_WL_DISPLAY_H__

#include <gtk/gtk.h>

#include "ctkconfig.h"
#include "displaycfg.h"

G_BEGIN_DECLS

#define CTK_TYPE_WL_DISPLAY (ctk_wl_display_get_type())

#define CTK_WL_DISPLAY(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), CTK_TYPE_WL_DISPLAY, CtkWlDisplay))

#define CTK_IS_WL_DISPLAY(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CTK_TYPE_WL_DISPLAY))

typedef struct _CtkWlDisplay       CtkWlDisplay;
typedef struct _CtkWlDisplayClass  CtkWlDisplayClass;

struct _CtkWlDisplay
{
    GtkVBox parent;

    CtkConfig *ctk_config;
    gchar *card;                 /* "card1" */
    gchar *connector;            /* "DP-1" */
    gchar *key;                  /* "card1-DP-1" */
    gchar *title;
    gchar *gpu_label;

    GtkWidget *status_label;
    GtkWidget *mode_label;
    GtkWidget *vrr_label;
    GtkWidget *bpc_label;
    GtkWidget *colorspace_label;
    GtkWidget *rgb_label;
    GtkWidget *format_label;
    GtkWidget *hdr_label;
    GtkListStore *mode_store;
    gchar *modes_sig;            /* detects mode list changes */

    /* configuration through the compositor (NULL backend: read-only) */
    DcBackend *backend;
    DcConnector *kms_info;       /* EDID identity, to find our output */
    gboolean unique_name;        /* no other GPU has this connector name */
    DcOutput *output;            /* last state read from the compositor */
    gboolean updating;

    GtkWidget *config_table;
    GtkWidget *config_status;
    GtkWidget *enable_check;
    GtkWidget *resolution_combo;
    GtkWidget *rate_combo;
    GtkWidget *scale_combo;
    GtkWidget *rotation_combo;
    GtkWidget *pos_x_spin;
    GtkWidget *pos_y_spin;
    GtkWidget *primary_check;
    GtkWidget *vrr_combo;
    GtkWidget *vrr_unsupported;
    GtkWidget *apply_button;
    GtkWidget *reset_button;

    GArray *resolutions;         /* DcMode: width, height, interlaced */
    GArray *rates;               /* gdouble, per rate combo row */
    GArray *scales;              /* gdouble, per scale combo row */
    GArray *vrr_values;          /* gint (DcVrr), per VRR combo row */

    /* Controls tab: color range and color space (pixel encoding) */
    GtkWidget *controls_table;
    GtkWidget *controls_status;
    GtkWidget *rgb_combo;
    GtkWidget *format_combo;
    GtkWidget *controls_apply;
    GtkWidget *controls_reset;
    GArray *rgb_values;          /* gint (DcRgbRange), per combo row */
    GArray *format_values;       /* gint (DcColorFormat), per combo row */
    guint verify_id;             /* checks the kernel took the settings */
    gint requested_rgb;          /* last applied, 0 if unchanged */
    gint requested_format;

    /* Color Correction tab */
    GtkWidget *cc_box;
    gchar *cc_output;            /* compositor output it was built for */
};

struct _CtkWlDisplayClass
{
    GtkVBoxClass parent_class;
};

GType          ctk_wl_display_get_type    (void) G_GNUC_CONST;

/* 'gpu_label' names the GPU driving the connector on the page */
GtkWidget*     ctk_wl_display_new         (const gchar *card,
                                           const gchar *connector,
                                           const gchar *gpu_label,
                                           CtkConfig *ctk_config);
GtkTextBuffer* ctk_wl_display_create_help (GtkTextTagTable *,
                                           CtkWlDisplay *);

void           ctk_wl_display_refresh     (CtkWlDisplay *);
void           ctk_wl_display_start_timer (GtkWidget *);
void           ctk_wl_display_stop_timer  (GtkWidget *);

G_END_DECLS

#endif /* __CTK_WL_DISPLAY_H__ */
