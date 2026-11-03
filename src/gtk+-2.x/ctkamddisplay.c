/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon display device page.  Displays connected to an amdgpu GPU are
 * not visible through NV-CONTROL, so this page drives them through the
 * standard XRandR extension (served by the modesetting or amdgpu X driver):
 * mode, refresh rate, rotation/reflection, position, primary output,
 * enable/disable, per-CRTC gamma/brightness and the connector properties the
 * driver exposes (max bpc, Broadcast RGB, Colorspace, scaling, underscan...).
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

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

#include "ctkamddisplay.h"
#include "ctkbanner.h"
#include "ctkhelp.h"
#include "ctkutils.h"

#define CONFIRM_TIMEOUT_SECONDS 15

typedef struct _CtkAmdResolution {
    guint width;
    guint height;
    gboolean interlaced;
} CtkAmdResolution;

typedef struct _CtkAmdProp {
    CtkAmdDisplay *page;
    Atom atom;
    Atom type;            /* XA_ATOM or XA_INTEGER */
    gboolean range;
    long *values;         /* enum values (atoms or integers) */
    gint num_values;
    GtkWidget *widget;    /* combo box or spin button */
} CtkAmdProp;

/* Connector properties that are internal, read-only in practice, or
 * dangerous to expose as generic controls. */
static const gchar *hidden_props[] = {
    "EDID", "CONNECTOR_ID", "link-status", "CTM", "non-desktop",
    "Content Protection", "HDCP Content Type", "GUID", "TILE", "PATH",
    "subconnector", "vrr_capable", "Backlight", "BACKLIGHT",
    "DEGAMMA_LUT", "GAMMA_LUT", "DEGAMMA_LUT_SIZE", "GAMMA_LUT_SIZE",
    "privacy-screen hw-state", "panel orientation",
    NULL
};

/* Friendlier labels for well-known DRM connector properties */
static const struct { const gchar *prop; const gchar *label; } prop_labels[] = {
    { "max bpc",           "Maximum Color Depth (bpc)" },
    { "Broadcast RGB",     "RGB Range" },
    { "Colorspace",        "Colorspace" },
    { "scaling mode",      "Scaling Mode" },
    { "underscan",         "Underscan" },
    { "underscan hborder", "Underscan Horizontal Border" },
    { "underscan vborder", "Underscan Vertical Border" },
    { "TearFree",          "TearFree" },
    { "audio",             "Audio" },
    { "content type",      "Content Type" },
    { "dither",            "Dithering" },
    { "dither depth",      "Dithering Depth" },
    { NULL, NULL }
};

static const struct { Rotation rot; const gchar *label; } rotations[] = {
    { RR_Rotate_0,   "Normal" },
    { RR_Rotate_90,  "Left" },
    { RR_Rotate_180, "Inverted" },
    { RR_Rotate_270, "Right" },
};
#define NUM_ROTATIONS 4

static const char *__enable_help =
"Turns this display on or off.";
static const char *__resolution_help =
"Selects the resolution of this display.  Click Apply to activate it.";
static const char *__rate_help =
"Selects the refresh rate for the chosen resolution.";
static const char *__rotation_help =
"Rotates the image shown on this display.";
static const char *__reflect_help =
"Mirrors the image shown on this display along the X or Y axis.";
static const char *__position_help =
"Position of the top left corner of this display within the X screen, in "
"pixels.";
static const char *__primary_help =
"Makes this the primary display; desktop environments place panels and new "
"windows here.";
static const char *__apply_help =
"Applies the resolution, refresh rate, rotation, position and enabled "
"state.  You will be asked to confirm the change; if you do not confirm "
"within 15 seconds the previous configuration is restored.";
static const char *__brightness_help =
"Adjusts the brightness of the image by scaling the CRTC gamma ramp.  This "
"is a software adjustment and does not change the monitor's backlight.";
static const char *__gamma_help =
"Adjusts the gamma correction applied to the red, green and blue channels.";
static const char *__props_help =
"Connector properties exposed by the kernel driver.  Changes are applied "
"immediately; some (such as maximum color depth or colorspace) only take "
"effect on the next mode set, which you can trigger with Apply above.";
static const char *__vrr_help =
"Whether the monitor and GPU support Variable Refresh Rate (AMD FreeSync / "
"VESA Adaptive-Sync).  With the modesetting driver, enable it by adding "
"Option \"VariableRefresh\" \"true\" to the Device section of xorg.conf; "
"with the amdgpu X driver use the same option name.  The compositor or "
"application must also request VRR.";


GType ctk_amd_display_get_type(void)
{
    static GType type = 0;

    if (!type) {
        static const GTypeInfo info = {
            sizeof (CtkAmdDisplayClass),
            NULL, NULL, NULL, NULL, NULL,
            sizeof (CtkAmdDisplay),
            0, NULL, NULL
        };
        type = g_type_register_static(GTK_TYPE_VBOX, "CtkAmdDisplay", &info, 0);
    }
    return type;
}


/* ---- X helpers ---------------------------------------------------------- */

static void trap_push(void)
{
#if GTK_CHECK_VERSION(3, 0, 0)
    gdk_x11_display_error_trap_push(gdk_display_get_default());
#else
    gdk_error_trap_push();
#endif
}

static gint trap_pop(void)
{
#if GTK_CHECK_VERSION(3, 0, 0)
    return gdk_x11_display_error_trap_pop(gdk_display_get_default());
#else
    gdk_flush();
    return gdk_error_trap_pop();
#endif
}


Display *ctk_amd_display_get_xdisplay(void)
{
    GdkDisplay *gdpy = gdk_display_get_default();
    Display *dpy;
    int ev, err, major = 0, minor = 0;

    if (!gdpy) {
        return NULL;
    }
#if GTK_CHECK_VERSION(3, 0, 0)
    if (!GDK_IS_X11_DISPLAY(gdpy)) {
        return NULL;
    }
#endif
    dpy = GDK_DISPLAY_XDISPLAY(gdpy);
    if (!dpy || !XRRQueryExtension(dpy, &ev, &err) ||
        !XRRQueryVersion(dpy, &major, &minor) ||
        major < 1 || (major == 1 && minor < 3)) {
        return NULL;
    }
    return dpy;
}


static double mode_refresh(const XRRModeInfo *m)
{
    double v = m->vTotal;

    if (m->modeFlags & RR_DoubleScan) v *= 2;
    if (m->modeFlags & RR_Interlace) v /= 2;
    if (m->hTotal == 0 || v == 0) {
        return 0;
    }
    return (double) m->dotClock / ((double) m->hTotal * v);
}


static XRRModeInfo *find_mode(XRRScreenResources *res, RRMode id)
{
    int i;
    for (i = 0; i < res->nmode; i++) {
        if (res->modes[i].id == id) {
            return &res->modes[i];
        }
    }
    return NULL;
}


static guchar *get_output_property(Display *dpy, RROutput output,
                                   const char *name, Atom req_type,
                                   gint *nitems_out, gint *format_out,
                                   Atom *type_out)
{
    Atom atom = XInternAtom(dpy, name, True);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (atom == None) {
        return NULL;
    }
    if (XRRGetOutputProperty(dpy, output, atom, 0, 1024, False, False,
                             req_type, &actual_type, &actual_format,
                             &nitems, &bytes_after, &prop) != Success) {
        return NULL;
    }
    if (actual_type == None || nitems == 0) {
        if (prop) XFree(prop);
        return NULL;
    }
    if (nitems_out) *nitems_out = nitems;
    if (format_out) *format_out = actual_format;
    if (type_out) *type_out = actual_type;
    return prop;
}


static gboolean get_output_int_property(Display *dpy, RROutput output,
                                        const char *name, long *value)
{
    gint n = 0, fmt = 0;
    guchar *data = get_output_property(dpy, output, name, XA_INTEGER,
                                       &n, &fmt, NULL);
    if (!data) {
        return FALSE;
    }
    if (fmt == 32) {
        *value = ((long *) data)[0];
    } else if (fmt == 16) {
        *value = ((short *) data)[0];
    } else {
        *value = ((char *) data)[0];
    }
    XFree(data);
    return TRUE;
}


/* Read a sysfs EDID into a newly allocated buffer */
static guchar *read_sysfs_edid(AmdConnector *c, gsize *len)
{
    gchar *path, *data = NULL;

    if (!c) {
        return NULL;
    }
    path = g_build_filename(c->sysfs_path, "edid", NULL);
    if (!g_file_get_contents(path, &data, len, NULL) || *len < 128) {
        g_free(data);
        data = NULL;
    }
    g_free(path);
    return (guchar *) data;
}


GList *ctk_amd_display_find_outputs(Display *dpy, AmdGpu *gpu,
                                    guint num_amd_gpus)
{
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources *res;
    GHashTable *nvidia_outputs;
    GList *list = NULL;
    int i, major = 0, minor = 0;

    res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) {
        return NULL;
    }

    /* Outputs that belong to an NVIDIA RandR provider are handled by the
     * regular NVIDIA pages; exclude them. */
    nvidia_outputs = g_hash_table_new(g_direct_hash, g_direct_equal);
    XRRQueryVersion(dpy, &major, &minor);
    if (major > 1 || (major == 1 && minor >= 4)) {
        XRRProviderResources *pr = XRRGetProviderResources(dpy, root);
        if (pr) {
            for (i = 0; i < pr->nproviders; i++) {
                XRRProviderInfo *pi =
                    XRRGetProviderInfo(dpy, res, pr->providers[i]);
                int j;
                if (!pi) continue;
                if (pi->name && g_ascii_strncasecmp(pi->name, "NVIDIA", 6) == 0) {
                    for (j = 0; j < pi->noutputs; j++) {
                        g_hash_table_insert(nvidia_outputs,
                                            GUINT_TO_POINTER(pi->outputs[j]),
                                            GUINT_TO_POINTER(1));
                    }
                }
                XRRFreeProviderInfo(pi);
            }
            XRRFreeProviderResources(pr);
        }
    }

    for (i = 0; i < res->noutput; i++) {
        RROutput out = res->outputs[i];
        XRROutputInfo *oi;
        GList *l;
        gboolean match = FALSE;
        long conn_id = -1;
        guchar *edid;
        gint edid_len = 0, fmt = 0;

        if (g_hash_table_lookup(nvidia_outputs, GUINT_TO_POINTER(out))) {
            continue;
        }
        oi = XRRGetOutputInfo(dpy, res, out);
        if (!oi) continue;
        if (oi->connection != RR_Connected) {
            XRRFreeOutputInfo(oi);
            continue;
        }
        XRRFreeOutputInfo(oi);

        get_output_int_property(dpy, out, "CONNECTOR_ID", &conn_id);
        edid = get_output_property(dpy, out, "EDID", AnyPropertyType,
                                   &edid_len, &fmt, NULL);
        if (fmt != 8) {
            if (edid) XFree(edid);
            edid = NULL;
            edid_len = 0;
        }

        for (l = gpu->connectors; l && !match; l = l->next) {
            AmdConnector *c = l->data;
            gsize slen = 0;
            guchar *sedid = read_sysfs_edid(c, &slen);
            gboolean edid_equal = (sedid && edid && slen == (gsize) edid_len &&
                                   memcmp(sedid, edid, slen) == 0);
            gboolean id_equal = (conn_id >= 0 && c->connector_id == conn_id);

            /* DRM connector ids are only unique per device, so with several
             * AMD GPUs require the EDID to agree as well when possible. */
            if (edid_equal && (id_equal || conn_id < 0)) {
                match = TRUE;
            } else if (id_equal && (num_amd_gpus == 1 || !edid || !sedid)) {
                match = TRUE;
            }
            g_free(sedid);
        }
        if (edid) XFree(edid);

        if (match) {
            list = g_list_append(list, GUINT_TO_POINTER(out));
        }
    }

    g_hash_table_destroy(nvidia_outputs);
    XRRFreeScreenResources(res);
    return list;
}


static AmdConnector *find_connector(Display *dpy, RROutput output, AmdGpu *gpu)
{
    long conn_id = -1;
    GList *l;

    if (get_output_int_property(dpy, output, "CONNECTOR_ID", &conn_id)) {
        for (l = gpu->connectors; l; l = l->next) {
            AmdConnector *c = l->data;
            if (c->connector_id == conn_id) {
                return c;
            }
        }
    }
    return NULL;
}


/* ---- screen state snapshot / restore ------------------------------------ */

typedef struct {
    RRCrtc crtc;
    RRMode mode;
    int x, y;
    Rotation rotation;
    RROutput *outputs;
    int noutputs;
} CrtcState;

typedef struct {
    int width, height, mm_width, mm_height;
    RROutput primary;
    int ncrtc;
    CrtcState *crtcs;
} ScreenState;


static ScreenState *snapshot_screen(Display *dpy, Window root)
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    ScreenState *s;
    int i, scr = DefaultScreen(dpy);

    if (!res) {
        return NULL;
    }
    s = g_new0(ScreenState, 1);
    s->width = DisplayWidth(dpy, scr);
    s->height = DisplayHeight(dpy, scr);
    s->mm_width = DisplayWidthMM(dpy, scr);
    s->mm_height = DisplayHeightMM(dpy, scr);
    s->primary = XRRGetOutputPrimary(dpy, root);
    s->ncrtc = res->ncrtc;
    s->crtcs = g_new0(CrtcState, res->ncrtc);

    for (i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
        CrtcState *cs = &s->crtcs[i];
        cs->crtc = res->crtcs[i];
        if (ci) {
            cs->mode = ci->mode;
            cs->x = ci->x;
            cs->y = ci->y;
            cs->rotation = ci->rotation;
            cs->noutputs = ci->noutput;
            cs->outputs = g_memdup2(ci->outputs,
                                    sizeof(RROutput) * ci->noutput);
            XRRFreeCrtcInfo(ci);
        }
    }
    XRRFreeScreenResources(res);
    return s;
}


static void free_screen_state(ScreenState *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < s->ncrtc; i++) {
        g_free(s->crtcs[i].outputs);
    }
    g_free(s->crtcs);
    g_free(s);
}


static void restore_screen(Display *dpy, Window root, ScreenState *s)
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    int i;

    if (!res || !s) {
        if (res) XRRFreeScreenResources(res);
        return;
    }

    trap_push();
    XGrabServer(dpy);

    /* Turn everything off that will change, so the screen can be resized */
    for (i = 0; i < s->ncrtc; i++) {
        XRRSetCrtcConfig(dpy, res, s->crtcs[i].crtc, CurrentTime, 0, 0, None,
                         RR_Rotate_0, NULL, 0);
    }
    XRRSetScreenSize(dpy, root, s->width, s->height, s->mm_width,
                     s->mm_height);
    for (i = 0; i < s->ncrtc; i++) {
        CrtcState *cs = &s->crtcs[i];
        if (cs->mode != None && cs->noutputs > 0) {
            XRRSetCrtcConfig(dpy, res, cs->crtc, CurrentTime, cs->x, cs->y,
                             cs->mode, cs->rotation, cs->outputs,
                             cs->noutputs);
        }
    }
    XRRSetOutputPrimary(dpy, root, s->primary);

    XUngrabServer(dpy);
    XSync(dpy, False);
    trap_pop();
    XRRFreeScreenResources(res);
}


/* ---- applying a configuration ------------------------------------------- */

static void crtc_extent(int w, int h, Rotation rot, int *ew, int *eh)
{
    if (rot & (RR_Rotate_90 | RR_Rotate_270)) {
        *ew = h;
        *eh = w;
    } else {
        *ew = w;
        *eh = h;
    }
}


/*
 * Apply the requested configuration for one output.  Returns NULL on success
 * or an error string.
 */
static gchar *apply_output_config(CtkAmdDisplay *page, gboolean enable,
                                  RRMode mode, Rotation rotation,
                                  int x, int y, gboolean primary,
                                  gboolean *touched)
{
    Display *dpy = page->dpy;
    Window root = page->root;
    XRRScreenResources *res;
    XRROutputInfo *oi;
    RRCrtc crtc = None;
    gchar *error = NULL;
    int scr = DefaultScreen(dpy);
    int min_w, min_h, max_w, max_h;
    int cur_w = DisplayWidth(dpy, scr), cur_h = DisplayHeight(dpy, scr);
    int bb_w = 0, bb_h = 0;
    double mm_per_px_w, mm_per_px_h;
    int i, xerr;
    Status st = RRSetConfigSuccess;
    RROutput *outs = NULL;
    int nouts = 0;

    *touched = FALSE;
    res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) {
        return g_strdup("Unable to query the XRandR screen resources.");
    }
    oi = XRRGetOutputInfo(dpy, res, page->output);
    if (!oi) {
        XRRFreeScreenResources(res);
        return g_strdup("Unable to query the output.");
    }

    mm_per_px_w = cur_w > 0 ? (double) DisplayWidthMM(dpy, scr) / cur_w
                            : 25.4 / 96;
    mm_per_px_h = cur_h > 0 ? (double) DisplayHeightMM(dpy, scr) / cur_h
                            : 25.4 / 96;
    if (!XRRGetScreenSizeRange(dpy, root, &min_w, &min_h, &max_w, &max_h)) {
        min_w = min_h = 1;
        max_w = max_h = 32767;
    }

    crtc = oi->crtc;
    if (enable && crtc == None) {
        /* Pick a free CRTC that can drive this output */
        for (i = 0; i < oi->ncrtc && crtc == None; i++) {
            XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtcs[i]);
            if (ci && ci->noutput == 0 && ci->mode == None) {
                crtc = oi->crtcs[i];
            }
            if (ci) XRRFreeCrtcInfo(ci);
        }
        if (crtc == None) {
            error = g_strdup("No free display controller (CRTC) is available "
                             "for this output.  Disable another display "
                             "first.");
            goto done;
        }
    }

    /* Compute the bounding box of the resulting layout */
    for (i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo *ci;
        if (res->crtcs[i] == crtc) continue;
        ci = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
        if (ci && ci->mode != None) {
            bb_w = MAX(bb_w, ci->x + (int) ci->width);
            bb_h = MAX(bb_h, ci->y + (int) ci->height);
        }
        if (ci) XRRFreeCrtcInfo(ci);
    }
    if (enable) {
        XRRModeInfo *mi = find_mode(res, mode);
        int ew, eh;
        if (!mi) {
            error = g_strdup("The selected mode is no longer available.");
            goto done;
        }
        crtc_extent(mi->width, mi->height, rotation, &ew, &eh);
        bb_w = MAX(bb_w, x + ew);
        bb_h = MAX(bb_h, y + eh);
    }
    if (bb_w == 0 || bb_h == 0) {
        error = g_strdup("Refusing to disable the last active display.");
        goto done;
    }
    if (bb_w > max_w || bb_h > max_h) {
        error = g_strdup_printf("The resulting screen size %dx%d exceeds the "
                                "maximum of %dx%d supported by the X server.",
                                bb_w, bb_h, max_w, max_h);
        goto done;
    }
    bb_w = MAX(bb_w, min_w);
    bb_h = MAX(bb_h, min_h);

    /* Keep any clones already sharing the CRTC */
    if (enable) {
        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, crtc);
        gboolean have_self = FALSE;
        outs = g_new0(RROutput, (ci ? ci->noutput : 0) + 1);
        if (ci) {
            for (i = 0; i < ci->noutput; i++) {
                outs[nouts++] = ci->outputs[i];
                if (ci->outputs[i] == page->output) have_self = TRUE;
            }
            XRRFreeCrtcInfo(ci);
        }
        if (!have_self) {
            outs[nouts++] = page->output;
        }
    }

    *touched = TRUE;
    trap_push();
    XGrabServer(dpy);

    if (bb_w > cur_w || bb_h > cur_h) {
        XRRSetScreenSize(dpy, root, MAX(bb_w, cur_w), MAX(bb_h, cur_h),
                         (int) (MAX(bb_w, cur_w) * mm_per_px_w + 0.5),
                         (int) (MAX(bb_h, cur_h) * mm_per_px_h + 0.5));
    }

    if (enable) {
        st = XRRSetCrtcConfig(dpy, res, crtc, CurrentTime, x, y, mode,
                              rotation, outs, nouts);
    } else if (crtc != None) {
        st = XRRSetCrtcConfig(dpy, res, crtc, CurrentTime, 0, 0, None,
                              RR_Rotate_0, NULL, 0);
    }

    if (st == RRSetConfigSuccess && (bb_w != cur_w || bb_h != cur_h)) {
        XRRSetScreenSize(dpy, root, bb_w, bb_h,
                         (int) (bb_w * mm_per_px_w + 0.5),
                         (int) (bb_h * mm_per_px_h + 0.5));
    }

    if (st == RRSetConfigSuccess) {
        RROutput cur_primary = XRRGetOutputPrimary(dpy, root);
        if (primary && enable) {
            XRRSetOutputPrimary(dpy, root, page->output);
        } else if (cur_primary == page->output) {
            XRRSetOutputPrimary(dpy, root, None);
        }
    }

    XUngrabServer(dpy);
    XSync(dpy, False);
    xerr = trap_pop();

    if (st != RRSetConfigSuccess) {
        error = g_strdup_printf("The X server rejected the configuration "
                                "(status %d).", st);
    } else if (xerr) {
        error = g_strdup_printf("The X server reported an error (%d) while "
                                "applying the configuration.", xerr);
    }

 done:
    g_free(outs);
    XRRFreeOutputInfo(oi);
    XRRFreeScreenResources(res);
    return error;
}


/* ---- confirmation dialog ------------------------------------------------ */

typedef struct {
    GtkWidget *dialog;
    GtkWidget *label;
    gint remaining;
} ConfirmData;

static void update_confirm_label(ConfirmData *cd)
{
    gchar *msg = g_strdup_printf("Do you want to keep this display "
                                 "configuration?\n\nReverting in %d "
                                 "second%s...", cd->remaining,
                                 cd->remaining == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(cd->label), msg);
    g_free(msg);
}

static gboolean confirm_tick(gpointer data)
{
    ConfirmData *cd = data;
    cd->remaining--;
    if (cd->remaining <= 0) {
        gtk_dialog_response(GTK_DIALOG(cd->dialog), GTK_RESPONSE_REJECT);
        return FALSE;
    }
    update_confirm_label(cd);
    return TRUE;
}

static gboolean confirm_configuration(CtkAmdDisplay *page)
{
    GtkWidget *parent = ctk_get_parent_window(GTK_WIDGET(page));
    ConfirmData cd;
    guint timer;
    gint response;

    cd.dialog = gtk_dialog_new_with_buttons(
        "Confirm Display Configuration",
        parent ? GTK_WINDOW(parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Revert", GTK_RESPONSE_REJECT,
        "_Keep", GTK_RESPONSE_ACCEPT,
        NULL);
    cd.label = gtk_label_new(NULL);
    cd.remaining = CONFIRM_TIMEOUT_SECONDS;
    gtk_misc_set_padding(GTK_MISC(cd.label), 15, 15);
    update_confirm_label(&cd);
    gtk_box_pack_start(
        GTK_BOX(ctk_dialog_get_content_area(GTK_DIALOG(cd.dialog))),
        cd.label, TRUE, TRUE, 0);
    gtk_widget_show_all(cd.dialog);

    timer = g_timeout_add(1000, confirm_tick, &cd);
    response = gtk_dialog_run(GTK_DIALOG(cd.dialog));
    if (cd.remaining > 0) {
        g_source_remove(timer);
    }
    gtk_widget_destroy(cd.dialog);

    return response == GTK_RESPONSE_ACCEPT;
}


/* ---- gamma -------------------------------------------------------------- */

static RRCrtc current_crtc(CtkAmdDisplay *page)
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(page->dpy,
                                                           page->root);
    XRROutputInfo *oi;
    RRCrtc crtc = None;

    if (!res) return None;
    oi = XRRGetOutputInfo(page->dpy, res, page->output);
    if (oi) {
        crtc = oi->crtc;
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    return crtc;
}


static void apply_gamma(CtkAmdDisplay *page)
{
    RRCrtc crtc = current_crtc(page);
    XRRCrtcGamma *g;
    double bright, gam[3];
    int size, i, c;

    if (crtc == None) return;
    size = XRRGetCrtcGammaSize(page->dpy, crtc);
    if (size <= 1) return;

    bright = gtk_adjustment_get_value(page->brightness_adj);
    for (c = 0; c < 3; c++) {
        gam[c] = gtk_adjustment_get_value(page->gamma_adj[c]);
        if (gam[c] <= 0) gam[c] = 1.0;
    }

    g = XRRAllocGamma(size);
    if (!g) return;
    for (i = 0; i < size; i++) {
        double x = (double) i / (size - 1);
        unsigned short *ramps[3] = { g->red, g->green, g->blue };
        for (c = 0; c < 3; c++) {
            double v = pow(x, 1.0 / gam[c]) * bright;
            if (v > 1.0) v = 1.0;
            if (v < 0.0) v = 0.0;
            ramps[c][i] = (unsigned short) (v * 65535.0 + 0.5);
        }
    }
    trap_push();
    XRRSetCrtcGamma(page->dpy, crtc, g);
    XSync(page->dpy, False);
    trap_pop();
    XRRFreeGamma(g);
}


/* Estimate brightness/gamma from the current ramp (same approach as
 * xrandr --verbose). */
static void read_gamma(CtkAmdDisplay *page)
{
    RRCrtc crtc = current_crtc(page);
    XRRCrtcGamma *g;
    int size, mid, c;
    double bright = 0;

    gtk_widget_set_sensitive(page->color_box, crtc != None);
    if (crtc == None) return;

    size = XRRGetCrtcGammaSize(page->dpy, crtc);
    if (size <= 2) return;
    g = XRRGetCrtcGamma(page->dpy, crtc);
    if (!g) return;

    {
        unsigned short *ramps[3] = { g->red, g->green, g->blue };
        mid = size / 2;
        for (c = 0; c < 3; c++) {
            bright = MAX(bright, ramps[c][size - 1] / 65535.0);
        }
        if (bright <= 0) bright = 1.0;
        for (c = 0; c < 3; c++) {
            double v = ramps[c][mid] / 65535.0 / bright;
            double gam = 1.0;
            if (v > 0 && v < 1) {
                double expo = log(v) / log((double) mid / (size - 1));
                if (expo > 0) gam = 1.0 / expo;
            }
            gtk_adjustment_set_value(page->gamma_adj[c], gam);
        }
    }
    gtk_adjustment_set_value(page->brightness_adj, bright);
    XRRFreeGamma(g);
}


static void gamma_changed(GtkAdjustment *adj, gpointer data)
{
    CtkAmdDisplay *page = CTK_AMD_DISPLAY(data);
    int c;

    if (page->updating) return;

    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->gamma_link_check))) {
        for (c = 0; c < 3; c++) {
            if (page->gamma_adj[c] == adj) {
                gdouble v = gtk_adjustment_get_value(adj);
                int k;
                page->updating = TRUE;
                for (k = 0; k < 3; k++) {
                    if (k != c) gtk_adjustment_set_value(page->gamma_adj[k], v);
                }
                page->updating = FALSE;
                break;
            }
        }
    }
    apply_gamma(page);
}


static void color_reset_clicked(GtkButton *b, gpointer data)
{
    CtkAmdDisplay *page = CTK_AMD_DISPLAY(data);
    int c;

    page->updating = TRUE;
    gtk_adjustment_set_value(page->brightness_adj, 1.0);
    for (c = 0; c < 3; c++) {
        gtk_adjustment_set_value(page->gamma_adj[c], 1.0);
    }
    page->updating = FALSE;
    apply_gamma(page);
    ctk_config_statusbar_message(page->ctk_config,
                                 "Color settings for %s reset.",
                                 page->output_name);
}


/* ---- mode widgets ------------------------------------------------------- */

static void fill_rates(CtkAmdDisplay *page, RRMode select_mode)
{
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(page->dpy,
                                                           page->root);
    XRROutputInfo *oi = res ? XRRGetOutputInfo(page->dpy, res, page->output)
                            : NULL;
    gint ridx = gtk_combo_box_get_active(GTK_COMBO_BOX(page->resolution_combo));
    GtkTreeModel *model =
        gtk_combo_box_get_model(GTK_COMBO_BOX(page->rate_combo));
    CtkAmdResolution *r;
    int i, active = -1, preferred_idx = -1;

    gboolean was = page->updating;
    page->updating = TRUE;

    gtk_list_store_clear(GTK_LIST_STORE(model));
    g_array_set_size(page->rate_modes, 0);

    if (!oi || ridx < 0 || ridx >= (gint) page->resolutions->len) {
        goto out;
    }
    r = &g_array_index(page->resolutions, CtkAmdResolution, ridx);

    for (i = 0; i < oi->nmode; i++) {
        XRRModeInfo *mi = find_mode(res, oi->modes[i]);
        gboolean il;
        gchar *text;
        if (!mi) continue;
        il = (mi->modeFlags & RR_Interlace) != 0;
        if (mi->width != r->width || mi->height != r->height ||
            il != r->interlaced) {
            continue;
        }
        text = g_strdup_printf("%.2f Hz%s%s", mode_refresh(mi),
                               i < oi->npreferred ? " (preferred)" : "",
                               (mi->nameLength && strchr(mi->name, '_')) ?
                               " (custom)" : "");
        ctk_combo_box_text_append_text(page->rate_combo, text);
        g_free(text);
        if (oi->modes[i] == select_mode) {
            active = page->rate_modes->len;
        }
        if (i < oi->npreferred && preferred_idx < 0) {
            preferred_idx = page->rate_modes->len;
        }
        g_array_append_val(page->rate_modes, oi->modes[i]);
    }

    if (active < 0) {
        /* pick preferred, else the highest refresh rate */
        if (preferred_idx >= 0) {
            active = preferred_idx;
        } else {
            double best = -1;
            for (i = 0; i < (int) page->rate_modes->len; i++) {
                XRRModeInfo *mi = find_mode(res,
                    g_array_index(page->rate_modes, RRMode, i));
                if (mi && mode_refresh(mi) > best) {
                    best = mode_refresh(mi);
                    active = i;
                }
            }
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(page->rate_combo), active);

 out:
    page->updating = was;
    if (oi) XRRFreeOutputInfo(oi);
    if (res) XRRFreeScreenResources(res);
}


static void resolution_changed(GtkComboBox *combo, gpointer data)
{
    CtkAmdDisplay *page = CTK_AMD_DISPLAY(data);
    if (page->updating) return;
    fill_rates(page, None);
}


static void enable_toggled(GtkToggleButton *b, gpointer data)
{
    CtkAmdDisplay *page = CTK_AMD_DISPLAY(data);
    gboolean on = gtk_toggle_button_get_active(b);
    gtk_widget_set_sensitive(page->resolution_combo, on);
    gtk_widget_set_sensitive(page->rate_combo, on);
    gtk_widget_set_sensitive(page->rotation_combo, on);
    gtk_widget_set_sensitive(page->reflect_x_check, on);
    gtk_widget_set_sensitive(page->reflect_y_check, on);
    gtk_widget_set_sensitive(page->pos_x_spin, on);
    gtk_widget_set_sensitive(page->pos_y_spin, on);
    gtk_widget_set_sensitive(page->primary_check, on);
}


static gint compare_resolutions(gconstpointer a, gconstpointer b)
{
    const CtkAmdResolution *ra = a, *rb = b;
    guint64 aa = (guint64) ra->width * ra->height;
    guint64 ab = (guint64) rb->width * rb->height;
    if (aa != ab) return aa < ab ? 1 : -1;
    if (ra->width != rb->width) return ra->width < rb->width ? 1 : -1;
    return ra->interlaced - rb->interlaced;
}


static void apply_clicked(GtkButton *b, gpointer data)
{
    CtkAmdDisplay *page = CTK_AMD_DISPLAY(data);
    gboolean enable =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->enable_check));
    gint rate_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(page->rate_combo));
    gint rot_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(page->rotation_combo));
    RRMode mode = None;
    Rotation rot = RR_Rotate_0;
    ScreenState *before;
    gchar *err;
    gboolean touched = FALSE;

    if (enable) {
        if (rate_idx < 0 || rate_idx >= (gint) page->rate_modes->len) {
            ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(page)),
                                  "Please select a resolution and refresh "
                                  "rate.");
            return;
        }
        mode = g_array_index(page->rate_modes, RRMode, rate_idx);
        if (rot_idx >= 0 && rot_idx < NUM_ROTATIONS) {
            rot = rotations[rot_idx].rot;
        }
        if (gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(page->reflect_x_check))) {
            rot |= RR_Reflect_X;
        }
        if (gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(page->reflect_y_check))) {
            rot |= RR_Reflect_Y;
        }
    }

    before = snapshot_screen(page->dpy, page->root);

    err = apply_output_config(
        page, enable, mode, rot,
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page->pos_x_spin)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page->pos_y_spin)),
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->primary_check)),
        &touched);

    if (err) {
        if (touched) {
            restore_screen(page->dpy, page->root, before);
        }
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(page)), err);
        g_free(err);
    } else if (!confirm_configuration(page)) {
        restore_screen(page->dpy, page->root, before);
        ctk_config_statusbar_message(page->ctk_config,
                                     "Display configuration for %s reverted.",
                                     page->output_name);
    } else {
        ctk_config_statusbar_message(page->ctk_config,
                                     "Display configuration for %s applied.",
                                     page->output_name);
    }

    free_screen_state(before);
    ctk_amd_display_refresh(page);
}


static void reset_clicked(GtkButton *b, gpointer data)
{
    ctk_amd_display_refresh(CTK_AMD_DISPLAY(data));
}


/* ---- output properties -------------------------------------------------- */

static gboolean prop_hidden(const char *name)
{
    int i;
    if (!name || name[0] == '_') return TRUE;
    for (i = 0; hidden_props[i]; i++) {
        if (strcmp(name, hidden_props[i]) == 0) return TRUE;
    }
    return FALSE;
}


static gchar *prop_label(const char *name)
{
    int i;
    for (i = 0; prop_labels[i].prop; i++) {
        if (strcmp(name, prop_labels[i].prop) == 0) {
            return g_strdup_printf("%s:", prop_labels[i].label);
        }
    }
    return g_strdup_printf("%c%s:", g_ascii_toupper(name[0]), name + 1);
}


static void prop_changed(GtkWidget *w, gpointer data)
{
    CtkAmdProp *p = data;
    CtkAmdDisplay *page = p->page;
    long value;
    int err;
    char *name;

    if (page->updating) return;

    if (p->range) {
        value = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p->widget));
    } else {
        gint idx = gtk_combo_box_get_active(GTK_COMBO_BOX(p->widget));
        if (idx < 0 || idx >= p->num_values) return;
        value = p->values[idx];
    }

    trap_push();
    XRRChangeOutputProperty(page->dpy, page->output, p->atom, p->type, 32,
                            PropModeReplace, (unsigned char *) &value, 1);
    XSync(page->dpy, False);
    err = trap_pop();

    name = XGetAtomName(page->dpy, p->atom);
    if (err) {
        gchar *msg = g_strdup_printf("The X server rejected the new value for "
                                     "'%s' on %s (X error %d).",
                                     name ? name : "?", page->output_name, err);
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(page)), msg);
        g_free(msg);
        ctk_amd_display_refresh(page);
    } else {
        ctk_config_statusbar_message(page->ctk_config, "Set %s on %s.",
                                     name ? name : "property",
                                     page->output_name);
    }
    if (name) XFree(name);
}


static void prop_refresh(CtkAmdProp *p)
{
    CtkAmdDisplay *page = p->page;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;
    long value;
    int i;

    if (XRRGetOutputProperty(page->dpy, page->output, p->atom, 0, 1, False,
                             False, AnyPropertyType, &actual_type,
                             &actual_format, &nitems, &bytes_after,
                             &data) != Success || !data || nitems < 1 ||
        actual_format != 32) {
        if (data) XFree(data);
        return;
    }
    value = ((long *) data)[0];
    XFree(data);

    if (p->range) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->widget), value);
    } else {
        for (i = 0; i < p->num_values; i++) {
            if (p->values[i] == value) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(p->widget), i);
                break;
            }
        }
    }
}


static void free_prop(gpointer data)
{
    CtkAmdProp *p = data;
    g_free(p->values);
    g_free(p);
}


static void page_destroyed(GObject *obj, gpointer data)
{
    CtkAmdDisplay *page = CTK_AMD_DISPLAY(obj);
    g_list_free_full(page->props, free_prop);
    page->props = NULL;
    if (page->resolutions) g_array_free(page->resolutions, TRUE);
    if (page->rate_modes) g_array_free(page->rate_modes, TRUE);
    page->resolutions = page->rate_modes = NULL;
    g_free(page->output_name);
    g_free(page->title);
    g_free(page->gpu_label);
    page->output_name = page->title = page->gpu_label = NULL;
}


static void build_properties(CtkAmdDisplay *page, GtkWidget *table, gint *row)
{
    Atom *atoms;
    int nprop = 0, i;

    atoms = XRRListOutputProperties(page->dpy, page->output, &nprop);
    if (!atoms) return;

    for (i = 0; i < nprop; i++) {
        char *name = XGetAtomName(page->dpy, atoms[i]);
        XRRPropertyInfo *info;
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;
        CtkAmdProp *p;
        GtkWidget *label, *hbox;
        gchar *ltext;
        int k;

        if (!name || prop_hidden(name)) {
            if (name) XFree(name);
            continue;
        }
        info = XRRQueryOutputProperty(page->dpy, page->output, atoms[i]);
        if (!info || info->immutable || info->num_values == 0) {
            if (info) XFree(info);
            XFree(name);
            continue;
        }
        if (XRRGetOutputProperty(page->dpy, page->output, atoms[i], 0, 1,
                                 False, False, AnyPropertyType, &actual_type,
                                 &actual_format, &nitems, &bytes_after,
                                 &data) != Success || nitems != 1 ||
            actual_format != 32 ||
            (actual_type != XA_ATOM && actual_type != XA_INTEGER) ||
            (info->range && (info->num_values != 2 || actual_type != XA_INTEGER))) {
            if (data) XFree(data);
            XFree(info);
            XFree(name);
            continue;
        }
        if (data) XFree(data);

        p = g_new0(CtkAmdProp, 1);
        p->page = page;
        p->atom = atoms[i];
        p->type = actual_type;
        p->range = info->range;
        p->num_values = info->num_values;
        p->values = g_memdup2(info->values, sizeof(long) * info->num_values);

        if (p->range) {
            p->widget = gtk_spin_button_new_with_range(p->values[0],
                                                       p->values[1], 1);
            g_signal_connect(G_OBJECT(p->widget), "value-changed",
                             G_CALLBACK(prop_changed), p);
        } else {
            p->widget = ctk_combo_box_text_new();
            for (k = 0; k < p->num_values; k++) {
                gchar *txt;
                if (p->type == XA_ATOM) {
                    char *an = XGetAtomName(page->dpy, p->values[k]);
                    txt = g_strdup(an ? an : "?");
                    if (an) XFree(an);
                } else {
                    txt = g_strdup_printf("%ld", p->values[k]);
                }
                ctk_combo_box_text_append_text(p->widget, txt);
                g_free(txt);
            }
            g_signal_connect(G_OBJECT(p->widget), "changed",
                             G_CALLBACK(prop_changed), p);
        }

        ltext = prop_label(name);
        label = gtk_label_new(ltext);
        g_free(ltext);
        gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
        hbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), p->widget, FALSE, FALSE, 0);
        gtk_table_resize(GTK_TABLE(table), *row + 1, 2);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, *row, *row + 1,
                         GTK_FILL, GTK_FILL, 0, 0);
        gtk_table_attach(GTK_TABLE(table), hbox, 1, 2, *row, *row + 1,
                         GTK_FILL, GTK_FILL, 0, 0);
        ctk_config_set_tooltip(page->ctk_config, p->widget, __props_help);
        (*row)++;

        page->props = g_list_append(page->props, p);

        XFree(info);
        XFree(name);
    }
    XFree(atoms);
}


/* ---- layout helpers ----------------------------------------------------- */

static void attach_row(CtkAmdDisplay *page, GtkWidget *table, gint *row,
                       const gchar *name, GtkWidget *control,
                       const gchar *help)
{
    GtkWidget *label = gtk_label_new(name);
    GtkWidget *hbox = gtk_hbox_new(FALSE, 5);

    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    gtk_table_resize(GTK_TABLE(table), *row + 1, 2);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, *row, *row + 1,
                     GTK_FILL, GTK_FILL, 0, 0);
    gtk_box_pack_start(GTK_BOX(hbox), control, FALSE, FALSE, 0);
    gtk_table_attach(GTK_TABLE(table), hbox, 1, 2, *row, *row + 1,
                     GTK_FILL, GTK_FILL, 0, 0);
    if (help) {
        ctk_config_set_tooltip(page->ctk_config, control, help);
    }
    (*row)++;
}


static GtkWidget *info_row(CtkAmdDisplay *page, GtkWidget *table, gint *row,
                           const gchar *name, const gchar *value,
                           const gchar *help)
{
    GtkWidget *l;
    gtk_table_resize(GTK_TABLE(table), *row + 1, 2);
    l = add_table_row_with_help_text(table, page->ctk_config, help, *row, 0,
                                     0.0f, 0.5f, name, 0.0f, 0.5f,
                                     value ? value : "Unknown");
    (*row)++;
    return l;
}


static GtkWidget *make_scale(CtkAmdDisplay *page, GtkAdjustment **adj,
                             gdouble lo, gdouble hi)
{
    GtkWidget *scale;

    *adj = GTK_ADJUSTMENT(gtk_adjustment_new(1.0, lo, hi, 0.01, 0.1, 0.0));
#if GTK_CHECK_VERSION(3, 0, 0)
    scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, *adj);
#else
    scale = gtk_hscale_new(*adj);
#endif
    gtk_scale_set_digits(GTK_SCALE(scale), 2);
    gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
    gtk_widget_set_size_request(scale, 250, -1);
    g_signal_connect(G_OBJECT(*adj), "value-changed",
                     G_CALLBACK(gamma_changed), page);
    return scale;
}


/* ---- refresh ------------------------------------------------------------ */

void ctk_amd_display_refresh(CtkAmdDisplay *page)
{
    XRRScreenResources *res;
    XRROutputInfo *oi;
    XRRCrtcInfo *ci = NULL;
    XRRModeInfo *cur = NULL;
    GtkTreeModel *model;
    gint i, ridx = -1;
    GList *l;
    long vrr = -1;

    res = XRRGetScreenResourcesCurrent(page->dpy, page->root);
    if (!res) return;
    oi = XRRGetOutputInfo(page->dpy, res, page->output);
    if (!oi) {
        XRRFreeScreenResources(res);
        return;
    }
    if (oi->crtc != None) {
        ci = XRRGetCrtcInfo(page->dpy, res, oi->crtc);
        if (ci && ci->mode != None) {
            cur = find_mode(res, ci->mode);
        }
    }

    page->updating = TRUE;

    /* current mode label */
    if (cur && ci) {
        gchar *s = g_strdup_printf("%ux%u%s @ %.2f Hz, +%d+%d",
                                   cur->width, cur->height,
                                   (cur->modeFlags & RR_Interlace) ? "i" : "",
                                   mode_refresh(cur), ci->x, ci->y);
        gtk_label_set_text(GTK_LABEL(page->current_mode_label), s);
        g_free(s);
    } else {
        gtk_label_set_text(GTK_LABEL(page->current_mode_label), "Disabled");
    }

    if (page->vrr_label) {
        if (get_output_int_property(page->dpy, page->output, "vrr_capable",
                                    &vrr)) {
            gtk_label_set_text(GTK_LABEL(page->vrr_label),
                               vrr ? "Supported" : "Not supported");
        } else {
            gtk_label_set_text(GTK_LABEL(page->vrr_label), "Unknown");
        }
    }

    /* resolutions */
    g_array_set_size(page->resolutions, 0);
    for (i = 0; i < oi->nmode; i++) {
        XRRModeInfo *mi = find_mode(res, oi->modes[i]);
        CtkAmdResolution r;
        guint k;
        gboolean dup = FALSE;
        if (!mi) continue;
        r.width = mi->width;
        r.height = mi->height;
        r.interlaced = (mi->modeFlags & RR_Interlace) != 0;
        for (k = 0; k < page->resolutions->len; k++) {
            CtkAmdResolution *e =
                &g_array_index(page->resolutions, CtkAmdResolution, k);
            if (e->width == r.width && e->height == r.height &&
                e->interlaced == r.interlaced) {
                dup = TRUE;
                break;
            }
        }
        if (!dup) g_array_append_val(page->resolutions, r);
    }
    g_array_sort(page->resolutions, compare_resolutions);

    model = gtk_combo_box_get_model(GTK_COMBO_BOX(page->resolution_combo));
    gtk_list_store_clear(GTK_LIST_STORE(model));
    for (i = 0; i < (gint) page->resolutions->len; i++) {
        CtkAmdResolution *r =
            &g_array_index(page->resolutions, CtkAmdResolution, i);
        gchar *txt = g_strdup_printf("%ux%u%s", r->width, r->height,
                                     r->interlaced ? " (interlaced)" : "");
        ctk_combo_box_text_append_text(page->resolution_combo, txt);
        g_free(txt);

        if (cur && r->width == cur->width && r->height == cur->height &&
            r->interlaced == ((cur->modeFlags & RR_Interlace) != 0)) {
            ridx = i;
        }
    }
    if (ridx < 0 && oi->npreferred > 0) {
        XRRModeInfo *pm = find_mode(res, oi->modes[0]);
        for (i = 0; pm && i < (gint) page->resolutions->len; i++) {
            CtkAmdResolution *r =
                &g_array_index(page->resolutions, CtkAmdResolution, i);
            if (r->width == pm->width && r->height == pm->height) {
                ridx = i;
                break;
            }
        }
    }
    if (ridx < 0 && page->resolutions->len > 0) ridx = 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(page->resolution_combo), ridx);
    fill_rates(page, cur ? cur->id : None);

    /* rotation / reflection */
    {
        Rotation r = ci ? ci->rotation : RR_Rotate_0;
        Rotation supported = ci ? ci->rotations : 0;
        gint rot_idx = 0;
        for (i = 0; i < NUM_ROTATIONS; i++) {
            if (r & rotations[i].rot) rot_idx = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(page->rotation_combo), rot_idx);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->reflect_x_check),
                                     (r & RR_Reflect_X) != 0);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->reflect_y_check),
                                     (r & RR_Reflect_Y) != 0);
        if (supported) {
            gtk_widget_set_sensitive(page->reflect_x_check,
                                     (supported & RR_Reflect_X) != 0);
            gtk_widget_set_sensitive(page->reflect_y_check,
                                     (supported & RR_Reflect_Y) != 0);
        }
    }

    /* position */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->pos_x_spin),
                              ci ? ci->x : 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->pos_y_spin),
                              ci ? ci->y : 0);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->primary_check),
                                 XRRGetOutputPrimary(page->dpy, page->root) ==
                                 page->output);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->enable_check),
                                 cur != NULL);
    enable_toggled(GTK_TOGGLE_BUTTON(page->enable_check), page);

    /* color + properties */
    read_gamma(page);
    for (l = page->props; l; l = l->next) {
        prop_refresh(l->data);
    }

    page->updating = FALSE;

    if (ci) XRRFreeCrtcInfo(ci);
    XRRFreeOutputInfo(oi);
    XRRFreeScreenResources(res);
}


/* ---- construction ------------------------------------------------------- */

GtkWidget *ctk_amd_display_new(Display *dpy, RROutput output, AmdGpu *gpu,
                               const gchar *gpu_label, CtkConfig *ctk_config)
{
    GObject *object;
    CtkAmdDisplay *page;
    XRRScreenResources *res;
    XRROutputInfo *oi;
    GtkWidget *banner, *vbox, *sw, *table, *hbox, *button, *w;
    gchar *monitor = NULL, *s;
    guchar *edid;
    gint edid_len = 0, fmt = 0, row, c;
    int i;

    res = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
    if (!res) return NULL;
    oi = XRRGetOutputInfo(dpy, res, output);
    if (!oi) {
        XRRFreeScreenResources(res);
        return NULL;
    }

    object = g_object_new(CTK_TYPE_AMD_DISPLAY, NULL);
    page = CTK_AMD_DISPLAY(object);
    page->ctk_config = ctk_config;
    page->dpy = dpy;
    page->root = DefaultRootWindow(dpy);
    page->output = output;
    page->output_name = g_strndup(oi->name, oi->nameLen);
    page->gpu = gpu;
    page->gpu_label = g_strdup(gpu_label ? gpu_label :
                               gpu ? gpu->product_name : NULL);
    page->connector = gpu ? find_connector(dpy, output, gpu) : NULL;
    page->resolutions = g_array_new(FALSE, FALSE, sizeof(CtkAmdResolution));
    page->rate_modes = g_array_new(FALSE, FALSE, sizeof(RRMode));

    edid = get_output_property(dpy, output, "EDID", AnyPropertyType,
                               &edid_len, &fmt, NULL);
    if (edid && fmt == 8) {
        monitor = amdgpu_edid_monitor_name(edid, edid_len);
    }
    if (edid) XFree(edid);
    if (!monitor && page->connector) {
        gsize len = 0;
        guchar *se = read_sysfs_edid(page->connector, &len);
        monitor = amdgpu_edid_monitor_name(se, len);
        g_free(se);
    }
    page->title = g_strdup_printf("%s - (%s)", page->output_name,
                                  monitor ? monitor : "Unknown Display");

    g_signal_connect(object, "destroy", G_CALLBACK(page_destroyed), NULL);

    gtk_box_set_spacing(GTK_BOX(page), 5);
    banner = ctk_banner_image_new(BANNER_ARTWORK_BLANK);
    gtk_box_pack_start(GTK_BOX(page), banner, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
                                        GTK_SHADOW_NONE);
    ctk_scrolled_window_add(GTK_SCROLLED_WINDOW(sw), vbox);
    gtk_box_pack_start(GTK_BOX(page), sw, TRUE, TRUE, 0);

    /* --- information --- */
    table = ctk_section_table_new(vbox, "Display Device Information");
    row = 0;
    info_row(page, table, &row, "Monitor:", monitor, NULL);
    info_row(page, table, &row, "RandR Output:", page->output_name, NULL);
    if (page->connector) {
        s = g_strdup_printf("%s-%s", gpu->card_name, page->connector->name);
        info_row(page, table, &row, "DRM Connector:", s, NULL);
        g_free(s);
    }
    info_row(page, table, &row, "GPU:", page->gpu_label, NULL);
    if (oi->mm_width && oi->mm_height) {
        double diag = sqrt((double) oi->mm_width * oi->mm_width +
                           (double) oi->mm_height * oi->mm_height) / 25.4;
        s = g_strdup_printf("%lu x %lu mm (%.1f\")", oi->mm_width,
                            oi->mm_height, diag);
        info_row(page, table, &row, "Physical Size:", s, NULL);
        g_free(s);
    }
    page->current_mode_label = info_row(page, table, &row, "Current Mode:",
                                        NULL, NULL);
    {
        long dummy;
        if (get_output_int_property(dpy, output, "vrr_capable", &dummy)) {
            page->vrr_label = info_row(page, table, &row,
                                       "Variable Refresh (FreeSync):", NULL,
                                       __vrr_help);
        }
    }

    /* --- configuration --- */
    table = ctk_section_table_new(vbox, "Display Configuration");
    page->config_table = table;
    row = 0;

    page->enable_check = gtk_check_button_new_with_label("Enabled");
    attach_row(page, table, &row, "Display:", page->enable_check,
               __enable_help);
    g_signal_connect(G_OBJECT(page->enable_check), "toggled",
                     G_CALLBACK(enable_toggled), page);

    page->resolution_combo = ctk_combo_box_text_new();
    attach_row(page, table, &row, "Resolution:", page->resolution_combo,
               __resolution_help);
    g_signal_connect(G_OBJECT(page->resolution_combo), "changed",
                     G_CALLBACK(resolution_changed), page);

    page->rate_combo = ctk_combo_box_text_new();
    attach_row(page, table, &row, "Refresh Rate:", page->rate_combo,
               __rate_help);

    page->rotation_combo = ctk_combo_box_text_new();
    for (i = 0; i < NUM_ROTATIONS; i++) {
        ctk_combo_box_text_append_text(page->rotation_combo,
                                       rotations[i].label);
    }
    attach_row(page, table, &row, "Rotation:", page->rotation_combo,
               __rotation_help);

    hbox = gtk_hbox_new(FALSE, 10);
    page->reflect_x_check = gtk_check_button_new_with_label("X axis");
    page->reflect_y_check = gtk_check_button_new_with_label("Y axis");
    gtk_box_pack_start(GTK_BOX(hbox), page->reflect_x_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), page->reflect_y_check, FALSE, FALSE, 0);
    attach_row(page, table, &row, "Reflection:", hbox, __reflect_help);

    hbox = gtk_hbox_new(FALSE, 5);
    page->pos_x_spin = gtk_spin_button_new_with_range(0, 32767, 1);
    page->pos_y_spin = gtk_spin_button_new_with_range(0, 32767, 1);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("X"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), page->pos_x_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new("Y"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), page->pos_y_spin, FALSE, FALSE, 0);
    attach_row(page, table, &row, "Position:", hbox, __position_help);

    page->primary_check =
        gtk_check_button_new_with_label("Make this the primary display");
    attach_row(page, table, &row, "Primary:", page->primary_check,
               __primary_help);

    hbox = gtk_hbox_new(FALSE, 5);
    button = gtk_button_new_with_label("Apply");
    gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    ctk_config_set_tooltip(ctk_config, button, __apply_help);
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(apply_clicked),
                     page);
    button = gtk_button_new_with_label("Reset");
    gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    ctk_config_set_tooltip(ctk_config, button,
                           "Discard unapplied changes and reload the current "
                           "configuration.");
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(reset_clicked),
                     page);
    gtk_table_resize(GTK_TABLE(table), row + 1, 2);
    gtk_table_attach(GTK_TABLE(table), hbox, 1, 2, row, row + 1,
                     GTK_FILL, GTK_FILL, 0, 5);
    row++;

    /* --- color correction --- */
    table = ctk_section_table_new(vbox, "Color Correction");
    page->color_box = table;
    row = 0;
    w = make_scale(page, &page->brightness_adj, 0.1, 2.0);
    attach_row(page, table, &row, "Brightness:", w, __brightness_help);
    {
        static const gchar *names[3] = { "Red Gamma:", "Green Gamma:",
                                         "Blue Gamma:" };
        for (c = 0; c < 3; c++) {
            w = make_scale(page, &page->gamma_adj[c], 0.3, 3.0);
            attach_row(page, table, &row, names[c], w, __gamma_help);
        }
    }
    hbox = gtk_hbox_new(FALSE, 10);
    page->gamma_link_check =
        gtk_check_button_new_with_label("Link color channels");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->gamma_link_check),
                                 TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), page->gamma_link_check, FALSE, FALSE, 0);
    button = gtk_button_new_with_label("Reset");
    gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(button), "clicked",
                     G_CALLBACK(color_reset_clicked), page);
    gtk_table_resize(GTK_TABLE(table), row + 1, 2);
    gtk_table_attach(GTK_TABLE(table), hbox, 1, 2, row, row + 1,
                     GTK_FILL, GTK_FILL, 0, 0);
    row++;

    /* --- connector properties --- */
    {
        GtkWidget *ptable = gtk_table_new(1, 2, FALSE);
        row = 0;
        build_properties(page, ptable, &row);
        if (row > 0) {
            GtkWidget *sec = ctk_section_table_new(vbox, "Output Properties");
            gtk_table_attach(GTK_TABLE(sec), ptable, 0, 2, 0, 1,
                             GTK_FILL, GTK_FILL, 0, 0);
            gtk_table_set_row_spacings(GTK_TABLE(ptable), 3);
            gtk_table_set_col_spacings(GTK_TABLE(ptable), 15);
        } else {
            gtk_widget_destroy(ptable);
        }
    }

    g_free(monitor);
    XRRFreeOutputInfo(oi);
    XRRFreeScreenResources(res);

    ctk_amd_display_refresh(page);
    gtk_widget_show_all(GTK_WIDGET(page));

    return GTK_WIDGET(page);
}


GtkTextBuffer *ctk_amd_display_create_help(GtkTextTagTable *table,
                                           CtkAmdDisplay *page)
{
    GtkTextIter i;
    GtkTextBuffer *b = gtk_text_buffer_new(table);

    gtk_text_buffer_get_iter_at_offset(b, &i, 0);

    ctk_help_title(b, &i, "%s Help", page->title);
    if (page->gpu) {
        ctk_help_para(b, &i, "This page controls a display connected to an "
                      "AMD Radeon GPU.  Such displays are not managed by the "
                      "NVIDIA X driver, so nvidia-settings configures them "
                      "through the standard XRandR extension.  Changes made "
                      "here are not saved to xorg.conf; use your desktop "
                      "environment's display settings or an xorg.conf "
                      "Monitor section to make them permanent.");
    } else {
        ctk_help_para(b, &i, "This page controls a display connected to the "
                      "NVIDIA GPU while another GPU drives the X screen.  "
                      "The NVIDIA GPU scans out an image rendered by the "
                      "display GPU (\"reverse PRIME\"), which requires the "
                      "NVIDIA RandR provider to use the display GPU as its "
                      "output source; see the PRIME page.  Such displays are "
                      "configured through the standard XRandR extension.  "
                      "Changes made here are not saved to xorg.conf; use your "
                      "desktop environment's display settings to make them "
                      "permanent.");
    }

    ctk_help_heading(b, &i, "Display");
    ctk_help_para(b, &i, "%s", __enable_help);
    ctk_help_heading(b, &i, "Resolution");
    ctk_help_para(b, &i, "%s", __resolution_help);
    ctk_help_heading(b, &i, "Refresh Rate");
    ctk_help_para(b, &i, "%s", __rate_help);
    ctk_help_heading(b, &i, "Rotation");
    ctk_help_para(b, &i, "%s", __rotation_help);
    ctk_help_heading(b, &i, "Reflection");
    ctk_help_para(b, &i, "%s", __reflect_help);
    ctk_help_heading(b, &i, "Position");
    ctk_help_para(b, &i, "%s", __position_help);
    ctk_help_heading(b, &i, "Primary");
    ctk_help_para(b, &i, "%s", __primary_help);
    ctk_help_heading(b, &i, "Apply");
    ctk_help_para(b, &i, "%s", __apply_help);
    ctk_help_heading(b, &i, "Brightness");
    ctk_help_para(b, &i, "%s", __brightness_help);
    ctk_help_heading(b, &i, "Gamma");
    ctk_help_para(b, &i, "%s", __gamma_help);
    if (page->vrr_label) {
        ctk_help_heading(b, &i, "Variable Refresh (FreeSync)");
        ctk_help_para(b, &i, "%s", __vrr_help);
    }
    if (page->props) {
        ctk_help_heading(b, &i, "Output Properties");
        ctk_help_para(b, &i, "%s", __props_help);
        ctk_help_term(b, &i, "Maximum Color Depth (bpc)");
        ctk_help_para(b, &i, "Upper limit for the bits per color channel sent "
                      "to the monitor.  Higher values allow 10-bit and HDR "
                      "output when the link bandwidth permits.");
        ctk_help_term(b, &i, "RGB Range");
        ctk_help_para(b, &i, "Full (0-255) or Limited (16-235) RGB range.  "
                      "Use Limited for TVs that expect video levels if "
                      "blacks look grey or washed out.");
        ctk_help_term(b, &i, "Colorspace");
        ctk_help_para(b, &i, "The colorimetry signalled to the monitor in "
                      "the InfoFrame.");
        ctk_help_term(b, &i, "Scaling Mode");
        ctk_help_para(b, &i, "How the GPU scales modes smaller than the "
                      "panel's native resolution.");
        ctk_help_term(b, &i, "Underscan");
        ctk_help_para(b, &i, "Shrinks the image to compensate for TVs that "
                      "overscan.  The border sizes set the margin in "
                      "pixels.");
    }

    ctk_help_finish(b);
    return b;
}


/* ---- RandR event watching ---------------------------------------------- */

typedef struct {
    Display *dpy;
    int event_base;
    GSourceFunc func;
    gpointer data;
    guint pending;
} RandrWatch;

static gboolean randr_debounced(gpointer user_data)
{
    RandrWatch *w = user_data;
    w->pending = 0;
    w->func(w->data);
    return FALSE;
}

static GdkFilterReturn randr_filter(GdkXEvent *gxevent, GdkEvent *event,
                                    gpointer user_data)
{
    XEvent *xev = (XEvent *) gxevent;
    RandrWatch *w = user_data;

    if (xev->type == w->event_base + RRScreenChangeNotify ||
        xev->type == w->event_base + RRNotify) {
        if (w->pending) {
            g_source_remove(w->pending);
        }
        w->pending = g_timeout_add(250, randr_debounced, w);
    }
    return GDK_FILTER_CONTINUE;
}

void ctk_amd_display_watch_randr(Display *dpy, GSourceFunc func, gpointer data)
{
    RandrWatch *w;
    int err;

    w = g_new0(RandrWatch, 1);
    w->dpy = dpy;
    w->func = func;
    w->data = data;
    if (!XRRQueryExtension(dpy, &w->event_base, &err)) {
        g_free(w);
        return;
    }
    /* GDK may already select some RandR events on the root window; this
     * mask is a superset of what it uses. */
    XRRSelectInput(dpy, DefaultRootWindow(dpy),
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                   RROutputChangeNotifyMask | RROutputPropertyNotifyMask);
    gdk_window_add_filter(NULL, randr_filter, w);
}
