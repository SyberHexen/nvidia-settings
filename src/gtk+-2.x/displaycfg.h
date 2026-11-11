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
    gchar   *color_format; /* "color format" (pixel encoding), NULL if
                            * unknown */
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

/* The color formats (bit mask of 1 << DcColorFormat) the monitor's EDID
 * allows for 'mode', judged as the kernel does: YCbCr 4:4:4 and 4:2:2 if it
 * supports them at all, YCbCr 4:2:0 only for the progressive modes the
 * CTA-861 4:2:0 data blocks list (in CTA extensions or embedded in
 * DisplayID).  Automatic and RGB are always allowed; all formats if there is
 * no EDID. */
guint          dc_edid_color_formats(const guchar *edid, gsize len,
                                     const DcMode *mode);

/* "16:9"; "1920x1080 (16:9)"; "1920x1080 (16:9) @ 60.00 Hz" */
gchar         *dc_aspect_ratio(guint width, guint height);
gchar         *dc_resolution_string(guint width, guint height,
                                    gboolean interlaced);
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


/* ---- display configuration backends ------------------------------------ */

/*
 * Wayland has no common protocol for changing display settings, so each
 * compositor family gets a backend:
 *
 *   GNOME (Mutter) and Cinnamon (Muffin)  DisplayConfig D-Bus interface
 *   KDE Plasma (KWin)                     kde_output_management_v2
 *   wlroots compositors and others        zwlr_output_manager_v1
 */

typedef enum {
    DC_TRANSFORM_NORMAL = 0,
    DC_TRANSFORM_90,
    DC_TRANSFORM_180,
    DC_TRANSFORM_270,
    DC_TRANSFORM_FLIPPED,
    DC_TRANSFORM_FLIPPED_90,
    DC_TRANSFORM_FLIPPED_180,
    DC_TRANSFORM_FLIPPED_270,
} DcTransform;

typedef enum {
    DC_VRR_UNSUPPORTED = -1,
    DC_VRR_NEVER = 0,
    DC_VRR_ALWAYS = 1,
    DC_VRR_AUTOMATIC = 2,     /* KDE: only for fullscreen applications */
} DcVrr;

/* The values match the DisplayConfig "rgb-range" and "color-format"
 * properties of Muffin */
typedef enum {
    DC_RGB_RANGE_AUTO = 1,
    DC_RGB_RANGE_FULL = 2,
    DC_RGB_RANGE_LIMITED = 3,
} DcRgbRange;

typedef enum {
    DC_COLOR_FORMAT_AUTO = 1,
    DC_COLOR_FORMAT_RGB = 2,
    DC_COLOR_FORMAT_YCBCR444 = 3,
    DC_COLOR_FORMAT_YCBCR422 = 4,
    DC_COLOR_FORMAT_YCBCR420 = 5,
} DcColorFormat;

typedef struct _DcOutputMode {
    gchar   *id;              /* backend identifier */
    guint    width;
    guint    height;
    gdouble  refresh;         /* Hz */
    gboolean preferred;
    gboolean interlaced;
    gboolean variable;        /* Mutter: variable refresh rate twin */
    GArray  *scales;          /* gdouble; NULL if any scale is allowed */
    gpointer handle;          /* backend object (wayland mode proxy) */
    guint    handle_id;       /* tells reused proxy addresses apart */
} DcOutputMode;

typedef struct _DcOutput {
    gchar   *name;            /* compositor's connector name ("DP-1") */
    gchar   *make;
    gchar   *model;
    gchar   *serial;
    gchar   *description;

    gboolean enabled;
    gint     x, y;
    gdouble  scale;
    DcTransform transform;
    gboolean primary;
    DcVrr    vrr;             /* current policy, or DC_VRR_UNSUPPORTED */
    guint    vrr_policies;    /* bit mask of (1 << DcVrr) allowed */
    gint     rgb_range;       /* DcRgbRange, 0 if it cannot be set */
    guint    rgb_ranges;      /* bit mask of (1 << DcRgbRange) allowed */
    gint     color_format;    /* DcColorFormat, 0 if it cannot be set */
    guint    color_formats;   /* bit mask of (1 << DcColorFormat) allowed */

    GArray  *modes;           /* DcOutputMode */
    gint     current_mode;    /* index into modes, -1 if none */

    gpointer handle;          /* backend object (wayland head proxy) */
    guint    handle_id;       /* tells reused proxy addresses apart */
} DcOutput;

enum {
    DC_CAP_PRIMARY         = 1 << 0,   /* outputs can be made primary */
    DC_CAP_ANY_SCALE       = 1 << 1,   /* scales are not restricted */
    DC_CAP_PERSIST         = 1 << 2,   /* changes can be saved by us */
    DC_CAP_DISABLE         = 1 << 3,   /* outputs can be turned off */
    DC_CAP_GLOBAL_SCALE    = 1 << 4,   /* all outputs share one scale */
    DC_CAP_LOGICAL_LAYOUT  = 1 << 5,   /* positions are in scaled units */
    DC_CAP_SAVED_BY_COMPOSITOR = 1 << 6, /* compositor saves on its own */
    DC_CAP_COMPOSITOR_CONFIRMS = 1 << 7, /* persistent changes are confirmed
                                          * (and reverted) by the desktop */
};

typedef struct _DcBackend DcBackend;

struct _DcBackend {
    const gchar *name;        /* "Mutter DisplayConfig" */
    guint caps;

    /* A fresh snapshot of every output (DcOutput*), or NULL with 'error'. */
    GList   *(*query)(DcBackend *b, GError **error);

    /* Apply a full configuration.  'persistent' asks the compositor to
     * remember it (only meaningful with DC_CAP_PERSIST). */
    gboolean (*apply)(DcBackend *b, GList *outputs, gboolean persistent,
                      GError **error);

    /* Gamma ramps of the output named 'output' (NULL if not supported).
     * The ramps have 'size' entries each; get returns newly allocated
     * ramps to g_free(). */
    gboolean (*get_gamma)(DcBackend *b, const gchar *output, guint *size,
                          guint16 **red, guint16 **green, guint16 **blue,
                          GError **error);
    gboolean (*set_gamma)(DcBackend *b, const gchar *output, guint size,
                          const guint16 *red, const guint16 *green,
                          const guint16 *blue, GError **error);

    GList   *watches;         /* internal: change callbacks */
    guint    notify_id;       /* internal: debounce source */
    GList   *cache;           /* internal: snapshot for dc_backend_outputs */
    gboolean cache_valid;

    gpointer priv;
};

/* The backend for the running compositor, or NULL (X11, or a compositor
 * without a supported configuration interface).  Cached. */
DcBackend     *dc_backend(void);

/* A copy of the current outputs, shared between callers until the
 * configuration changes; NULL with 'error' on failure. */
GList         *dc_backend_outputs(DcBackend *b, GError **error);

/* Apply a configuration (see DcBackend.apply) */
gboolean       dc_backend_apply(DcBackend *b, GList *outputs,
                                gboolean persistent, GError **error);

/* Register a callback for configuration changes; several may be added. */
void           dc_backend_add_watch(DcBackend *b, GSourceFunc func,
                                    gpointer data);
void           dc_backend_remove_watch(DcBackend *b, GSourceFunc func,
                                       gpointer data);

/* Backend constructors; each returns NULL if its interface is absent. */
DcBackend     *dc_backend_mutter_new(const gchar *bus_name,
                                     const gchar *object_path,
                                     const gchar *interface);
DcBackend     *dc_backend_kde_new(void);
DcBackend     *dc_backend_wlr_new(void);

/* Internal: notify watchers, debounced. */
void           dc_backend_notify_changed(DcBackend *b);

DcOutput      *dc_output_copy(const DcOutput *o);
void           dc_output_free(gpointer o);
void           dc_output_list_free(GList *list);
GList         *dc_output_list_copy(GList *list);

/* Size the output occupies in the layout (scaled and rotated as the
 * backend requires); 0x0 if it is disabled. */
void           dc_output_extent(const DcBackend *b, const DcOutput *o,
                                gint *w, gint *h);

/* The output in 'outputs' showing connector 'c', or NULL. */
DcOutput      *dc_output_for_connector(GList *outputs, const DcConnector *c,
                                       gboolean unique_name);

/*
 * Keep the layout free of gaps after 'changed' was edited ('before' is its
 * previous state).  Outputs to the right of / below its old area move by
 * the change in its size, which also closes the hole a disabled output
 * leaves.  An output that is turned on without being 'moved' is placed to
 * the right of the others.  Finally the layout is moved to start at 0,0.
 */
void           dc_layout_fixup(const DcBackend *b, GList *outputs,
                               DcOutput *changed, const DcOutput *before,
                               gboolean moved);

const gchar   *dc_transform_name(DcTransform t);

G_END_DECLS

#endif /* __DISPLAYCFG_H__ */
