/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * Display-server independent display information: session and compositor
 * detection, and a read-only view of the kernel's KMS state.
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

#ifndef __DISPLAYCFG_H__
#define __DISPLAYCFG_H__

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
    DC_SESSION_X11,        /* a real X server (Xorg) */
    DC_SESSION_WAYLAND,    /* a Wayland compositor (GUI on Wayland or on
                            * XWayland) */
    DC_SESSION_UNKNOWN,
} DcSessionType;

typedef struct _DcCompositor {
    guint  pid;            /* 0 if unknown */
    gchar *process;        /* "gnome-shell", "kwin_wayland", ... */
    gchar *desktop;        /* "GNOME", "KDE Plasma", "Cinnamon", ... */
    GList *cards;          /* DRM cards it has open ("card1"), gchar* */
} DcCompositor;

typedef struct _DcMode {
    guint    width;
    guint    height;
    gdouble  refresh;      /* Hz */
    gboolean interlaced;
    gboolean preferred;
} DcMode;

/* A DRM connector as the kernel reports it */
typedef struct _DcConnector {
    gchar   *card;         /* "card1" */
    gchar   *name;         /* "DP-1" (kernel naming) */
    guint32  connector_id;
    gboolean connected;
    gboolean non_desktop;  /* VR headsets and similar */

    gboolean active;       /* driven by a CRTC */
    DcMode   current;      /* valid when active */
    gint     crtc_x, crtc_y;
    GArray  *modes;        /* DcMode, as listed by the kernel */

    guint    width_mm, height_mm;
    gint     vrr_capable;  /* -1 unknown, else 0/1 */
    gint     vrr_enabled;  /* -1 unknown, else 0/1 */
    gint     max_bpc;      /* -1 unknown */
    gchar   *colorspace;   /* NULL if unknown */
    gchar   *rgb_range;    /* "Broadcast RGB", NULL if unknown */
    gint     hdr_metadata; /* -1 unknown, 1 if HDR metadata is being sent */

    guchar  *edid;
    gsize    edid_len;
    gchar   *monitor;      /* EDID monitor name, may be NULL */
    gchar   *vendor;       /* EDID PNP id ("HEC"), may be NULL */
    gchar   *serial;       /* EDID serial number string, may be NULL */
    guint16  product_code;
    gint     year;         /* EDID year of manufacture, 0 if unknown */
} DcConnector;


DcSessionType  dc_session_type(void);
const gchar   *dc_session_name(DcSessionType type);

/* Information about the Wayland compositor this process talks to, or NULL
 * if not running under Wayland.  Cached; do not free. */
const DcCompositor *dc_compositor(void);

/* TRUE if this build can read KMS state (built with libdrm). */
gboolean       dc_kms_available(void);

/* All connectors of DRM card 'card' ("card1"), as DcConnector*.  Does not
 * force a probe of the outputs, so it is cheap enough to poll. */
GList         *dc_kms_connectors(const gchar *card);

/* Only the connected, non-VR connectors. */
GList         *dc_kms_connected(const gchar *card);

DcConnector   *dc_kms_connector(const gchar *card, const gchar *name);

void           dc_connector_free(gpointer c);
void           dc_connector_list_free(GList *list);

/* A string that changes whenever any DRM connector is (un)plugged or turned
 * on or off.  Reads sysfs only: cheap, and never wakes a GPU. */
gchar         *dc_connector_state_signature(void);

/* "1920x1080 @ 60.00 Hz" */
gchar         *dc_mode_string(const DcMode *m);

/* "DP-1 - (HISENSE)" */
gchar         *dc_connector_title(const DcConnector *c);

/* Call 'func(data)' (debounced) when the kernel reports a DRM hotplug or
 * connector change.  Returns FALSE if hotplug events are not available. */
gboolean       dc_watch_hotplug(GSourceFunc func, gpointer data);

/* Launch the desktop environment's display settings.  Returns FALSE (and
 * sets 'error') if the desktop is unknown or the tool cannot be started. */
gboolean       dc_open_desktop_display_settings(GError **error);

/* Name of the desktop's display settings tool, or NULL if unknown. */
const gchar   *dc_desktop_display_settings_name(void);

G_END_DECLS

#endif /* __DISPLAYCFG_H__ */
