/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon display device page (X11 / XRandR based).
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

#ifndef __CTK_AMD_DISPLAY_H__
#define __CTK_AMD_DISPLAY_H__

#include <gtk/gtk.h>
#include <X11/Xlib.h>

#include "ctkconfig.h"
#include "amdgpu-sysfs.h"

G_BEGIN_DECLS

#define CTK_TYPE_AMD_DISPLAY (ctk_amd_display_get_type())

#define CTK_AMD_DISPLAY(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), CTK_TYPE_AMD_DISPLAY, CtkAmdDisplay))

#define CTK_IS_AMD_DISPLAY(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CTK_TYPE_AMD_DISPLAY))

typedef struct _CtkAmdDisplay       CtkAmdDisplay;
typedef struct _CtkAmdDisplayClass  CtkAmdDisplayClass;

struct _CtkAmdDisplay
{
    GtkVBox parent;

    CtkConfig *ctk_config;
    Display *dpy;
    Window root;
    XID output;                  /* RROutput */
    gchar *output_name;
    gchar *title;

    AmdGpu *gpu;                 /* not owned; NULL for NVIDIA outputs */
    gchar *gpu_label;            /* "card1 - (AMD Radeon ...)" */
    AmdConnector *connector;     /* not owned, may be NULL */

    /* information labels */
    GtkWidget *monitor_label;
    GtkWidget *current_mode_label;
    GtkWidget *vrr_label;

    /* configuration */
    GtkWidget *enable_check;
    GtkWidget *resolution_combo;
    GtkWidget *rate_combo;
    GtkWidget *rotation_combo;
    GtkWidget *reflect_x_check;
    GtkWidget *reflect_y_check;
    GtkWidget *pos_x_spin;
    GtkWidget *pos_y_spin;
    GtkWidget *primary_check;
    GtkWidget *config_table;

    GArray *resolutions;         /* CtkAmdResolution */
    GArray *rate_modes;          /* RRMode for the selected resolution */

    /* color */
    GtkWidget *color_box;
    GtkAdjustment *brightness_adj;
    GtkAdjustment *gamma_adj[3];
    GtkWidget *gamma_link_check;

    /* output properties */
    GList *props;                /* CtkAmdProp* */

    gboolean updating;
};

struct _CtkAmdDisplayClass
{
    GtkVBoxClass parent_class;
};

GType          ctk_amd_display_get_type    (void) G_GNUC_CONST;

/* Returns the X display to use for AMD display pages, or NULL if the GUI is
 * not running on an X11 display with XRandR >= 1.3. */
Display       *ctk_amd_display_get_xdisplay(void);

/* Returns a list of RROutput XIDs (as GUINT_TO_POINTER) for connected outputs that
 * are driven by 'gpu'. */
GList         *ctk_amd_display_find_outputs(Display *dpy, AmdGpu *gpu,
                                            guint num_amd_gpus);

/* 'gpu' may be NULL for outputs of a non-AMD GPU (e.g. NVIDIA reverse PRIME
 * outputs); 'gpu_label' names the GPU on the page (defaults to the AMD
 * product name). */
GtkWidget*     ctk_amd_display_new         (Display *dpy, XID output,
                                            AmdGpu *gpu,
                                            const gchar *gpu_label,
                                            CtkConfig *config);
GtkTextBuffer* ctk_amd_display_create_help (GtkTextTagTable *,
                                            CtkAmdDisplay *);

/* Re-read the current state from the X server into the widgets */
void           ctk_amd_display_refresh     (CtkAmdDisplay *);

/* Install an X event filter that calls 'func(data)' (debounced) whenever the
 * RandR configuration changes. */
void           ctk_amd_display_watch_randr (Display *dpy, GSourceFunc func,
                                            gpointer data);

G_END_DECLS

#endif /* __CTK_AMD_DISPLAY_H__ */
