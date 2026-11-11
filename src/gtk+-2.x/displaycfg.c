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

#define _GNU_SOURCE   /* struct ucred */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/netlink.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#ifdef HAVE_LIBDRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#include "displaycfg.h"
#include "amdgpu-sysfs.h"

#define DRM_SYSFS_DIR "/sys/class/drm"
#define HOTPLUG_DEBOUNCE_MS 500


/* ---- session ------------------------------------------------------------ */

static gboolean x_server_is_xwayland(Display *dpy)
{
    int op, ev, err;

    if (XQueryExtension(dpy, "XWAYLAND", &op, &ev, &err)) {
        return TRUE;
    }
    /* Older Xwayland releases lack the XWAYLAND extension; the compositor
     * still exports its socket to the clients it starts. */
    return g_getenv("WAYLAND_DISPLAY") != NULL;
}


DcSessionType dc_session_type(void)
{
    static gint cached = -1;
    GdkDisplay *gdpy;
    const gchar *type;

    if (cached >= 0) {
        return cached;
    }

    gdpy = gdk_display_get_default();
    if (gdpy) {
        const gchar *klass = G_OBJECT_TYPE_NAME(gdpy);

        if (strstr(klass, "Wayland")) {
            cached = DC_SESSION_WAYLAND;
            return cached;
        }
#if GTK_CHECK_VERSION(3, 0, 0)
        if (GDK_IS_X11_DISPLAY(gdpy))
#endif
        {
            Display *dpy = GDK_DISPLAY_XDISPLAY(gdpy);
            if (dpy) {
                cached = x_server_is_xwayland(dpy) ? DC_SESSION_WAYLAND
                                                   : DC_SESSION_X11;
                return cached;
            }
        }
    }

    type = g_getenv("XDG_SESSION_TYPE");
    if (g_getenv("WAYLAND_DISPLAY") || g_strcmp0(type, "wayland") == 0) {
        cached = DC_SESSION_WAYLAND;
    } else if (g_getenv("DISPLAY") || g_strcmp0(type, "x11") == 0) {
        cached = DC_SESSION_X11;
    } else {
        cached = DC_SESSION_UNKNOWN;
    }
    return cached;
}


const gchar *dc_session_name(DcSessionType type)
{
    switch (type) {
    case DC_SESSION_X11:     return "X11";
    case DC_SESSION_WAYLAND: return "Wayland";
    default:                 return "Unknown";
    }
}


/* ---- compositor --------------------------------------------------------- */

static const struct {
    const gchar *process;
    const gchar *desktop;
} compositors[] = {
    { "gnome-shell",   "GNOME" },
    { "mutter",        "GNOME" },
    { "kwin_wayland",  "KDE Plasma" },
    { "cinnamon",      "Cinnamon" },
    { "muffin",        "Cinnamon" },
    { "Hyprland",      "Hyprland" },
    { "sway",          "sway" },
    { "niri",          "niri" },
    { "labwc",         "labwc" },
    { "wayfire",       "Wayfire" },
    { "river",         "river" },
    { "cosmic-comp",   "COSMIC" },
    { "weston",        "Weston" },
    { "gamescope",     "Gamescope" },
    { NULL, NULL }
};


static guint wayland_compositor_pid(void)
{
    const gchar *name = g_getenv("WAYLAND_DISPLAY");
    const gchar *dir = g_getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr;
    struct ucred cred;
    socklen_t len = sizeof(cred);
    gchar *path;
    guint pid = 0;
    int fd;

    if (!name) {
        name = "wayland-0";
    }
    if (name[0] == '/') {
        path = g_strdup(name);
    } else if (dir) {
        path = g_build_filename(dir, name, NULL);
    } else {
        return 0;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        g_free(path);
        return 0;
    }
    strcpy(addr.sun_path, path);
    g_free(path);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0 &&
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) {
        pid = cred.pid;
    }
    close(fd);
    return pid;
}


static gchar *process_comm(guint pid)
{
    gchar *path = g_strdup_printf("/proc/%u/comm", pid);
    gchar *data = NULL;

    if (g_file_get_contents(path, &data, NULL, NULL)) {
        g_strstrip(data);
    }
    g_free(path);
    return data;
}


/* The DRM cards (primary nodes) that process 'pid' has open.  Only works for
 * processes of the same user, which compositors normally are. */
static GList *process_drm_cards(guint pid)
{
    gchar *fd_dir = g_strdup_printf("/proc/%u/fd", pid);
    GDir *dir = g_dir_open(fd_dir, 0, NULL);
    const gchar *entry;
    GList *cards = NULL;

    while (dir && (entry = g_dir_read_name(dir))) {
        gchar *link = g_build_filename(fd_dir, entry, NULL);
        gchar *target = g_file_read_link(link, NULL);

        if (target && g_str_has_prefix(target, "/dev/dri/card")) {
            const gchar *card = target + strlen("/dev/dri/");
            if (!g_list_find_custom(cards, card, (GCompareFunc) strcmp)) {
                cards = g_list_insert_sorted(cards, g_strdup(card),
                                             (GCompareFunc) strcmp);
            }
        }
        g_free(target);
        g_free(link);
    }
    if (dir) {
        g_dir_close(dir);
    }
    g_free(fd_dir);
    return cards;
}


static gchar *desktop_from_env(void)
{
    const gchar *d = g_getenv("XDG_CURRENT_DESKTOP");
    gchar **parts, *ret;

    if (!d || !*d) {
        return NULL;
    }
    /* "X-Cinnamon", "ubuntu:GNOME", "KDE" */
    parts = g_strsplit(d, ":", -1);
    ret = g_strdup(parts[g_strv_length(parts) - 1]);
    g_strfreev(parts);
    if (g_str_has_prefix(ret, "X-")) {
        gchar *s = g_strdup(ret + 2);
        g_free(ret);
        ret = s;
    }
    if (strcmp(ret, "KDE") == 0) {
        g_free(ret);
        ret = g_strdup("KDE Plasma");
    }
    return ret;
}


const DcCompositor *dc_compositor(void)
{
    static gboolean done = FALSE;
    static DcCompositor *comp = NULL;
    gint i;

    if (done) {
        return comp;
    }
    done = TRUE;

    if (dc_session_type() != DC_SESSION_WAYLAND) {
        return NULL;
    }

    comp = g_new0(DcCompositor, 1);
    comp->pid = wayland_compositor_pid();
    if (comp->pid) {
        comp->process = process_comm(comp->pid);
        comp->cards = process_drm_cards(comp->pid);
    }
    for (i = 0; comp->process && compositors[i].process; i++) {
        if (strcmp(comp->process, compositors[i].process) == 0) {
            comp->desktop = g_strdup(compositors[i].desktop);
            break;
        }
    }
    if (!comp->desktop) {
        comp->desktop = desktop_from_env();
    }
    return comp;
}


/* ---- EDID --------------------------------------------------------------- */

static void parse_edid(DcConnector *c)
{
    const guchar *e = c->edid;
    guint16 m;
    guint32 serial;

    if (!e || c->edid_len < 128 ||
        e[0] != 0x00 || e[1] != 0xFF || e[7] != 0x00) {
        return;
    }

    m = (e[8] << 8) | e[9];
    c->vendor = g_strdup_printf("%c%c%c", '@' + ((m >> 10) & 0x1f),
                                '@' + ((m >> 5) & 0x1f), '@' + (m & 0x1f));
    c->product_code = e[10] | (e[11] << 8);
    c->monitor = drm_edid_descriptor_string(e, c->edid_len, 0xFC);
    c->serial = drm_edid_descriptor_string(e, c->edid_len, 0xFF);
    serial = e[12] | (e[13] << 8) | (e[14] << 16) | ((guint32) e[15] << 24);
    if (!c->serial && serial) {
        c->serial = g_strdup_printf("%u", serial);
    }
    if (e[17] > 0 && e[17] != 0xFF) {
        c->year = 1990 + e[17];
    }
    if (!c->width_mm && e[21] && e[22]) {
        c->width_mm = e[21] * 10;
        c->height_mm = e[22] * 10;
    }
}


/* ---- connectors (sysfs) ------------------------------------------------- */

static gboolean is_card_name(const gchar *s)
{
    const gchar *p;

    if (!g_str_has_prefix(s, "card") || !s[4]) {
        return FALSE;
    }
    for (p = s + 4; *p; p++) {
        if (!g_ascii_isdigit(*p)) {
            return FALSE;
        }
    }
    return TRUE;
}


/* Binary sysfs files (EDID); text files use amdgpu_read_string() */
static gchar *read_file(const gchar *dir, const gchar *file, gsize *len)
{
    gchar *path = g_build_filename(dir, file, NULL);
    gchar *data = NULL;

    if (!g_file_get_contents(path, &data, len, NULL)) {
        data = NULL;
    }
    g_free(path);
    return data;
}


static gboolean card_runtime_suspended(const gchar *card)
{
    gchar *dir = g_build_filename(DRM_SYSFS_DIR, card, "device", "power",
                                  NULL);
    gchar *status = amdgpu_read_string(dir, "runtime_status");
    gboolean ret = status && g_str_has_prefix(status, "suspended");

    g_free(status);
    g_free(dir);
    return ret;
}


static gint compare_modes(gconstpointer a, gconstpointer b)
{
    const DcMode *x = a, *y = b;

    if (x->width * x->height != y->width * y->height) {
        return (x->width * x->height < y->width * y->height) ? 1 : -1;
    }
    if (x->width != y->width) {
        return x->width < y->width ? 1 : -1;
    }
    if (x->refresh != y->refresh) {
        return x->refresh < y->refresh ? 1 : -1;
    }
    return 0;
}


/* Connector state from sysfs only: never wakes the GPU, but has no current
 * mode or refresh rates. */
static DcConnector *sysfs_connector(const gchar *card, const gchar *entry)
{
    gchar *dir = g_build_filename(DRM_SYSFS_DIR, entry, NULL);
    gchar *s;
    gint64 id;
    DcConnector *c = g_new0(DcConnector, 1);

    c->card = g_strdup(card);
    c->name = g_strdup(entry + strlen(card) + 1);
    c->modes = g_array_new(FALSE, FALSE, sizeof(DcMode));
    c->vrr_capable = c->vrr_enabled = c->max_bpc = c->hdr_metadata = -1;

    if (amdgpu_read_int64(dir, "connector_id", &id)) {
        c->connector_id = (guint32) id;
    }
    s = amdgpu_read_string(dir, "status");
    c->connected = g_strcmp0(s, "connected") == 0;
    g_free(s);
    s = amdgpu_read_string(dir, "enabled");
    c->active = g_strcmp0(s, "enabled") == 0;
    g_free(s);

    s = amdgpu_read_string(dir, "modes");
    if (s) {
        gchar **lines = g_strsplit(s, "\n", -1);
        gint i;
        for (i = 0; lines[i]; i++) {
            DcMode m = { 0 };
            if (sscanf(lines[i], "%ux%u", &m.width, &m.height) == 2) {
                m.interlaced = strchr(lines[i], 'i') != NULL;
                m.preferred = (c->modes->len == 0);
                g_array_append_val(c->modes, m);
            }
        }
        g_strfreev(lines);
        g_free(s);
    }

    c->edid = (guchar *) read_file(dir, "edid", &c->edid_len);
    if (c->edid && c->edid_len < 128) {
        g_free(c->edid);
        c->edid = NULL;
        c->edid_len = 0;
    }
    parse_edid(c);

    g_free(dir);
    return c;
}


/* ---- connectors (KMS) --------------------------------------------------- */

gboolean dc_kms_available(void)
{
#ifdef HAVE_LIBDRM
    return TRUE;
#else
    return FALSE;
#endif
}


#ifdef HAVE_LIBDRM

static gdouble mode_refresh(const drmModeModeInfo *m)
{
    gdouble r;

    if (!m->htotal || !m->vtotal) {
        return m->vrefresh;
    }
    r = (m->clock * 1000.0) / ((gdouble) m->htotal * m->vtotal);
    if (m->flags & DRM_MODE_FLAG_INTERLACE) r *= 2.0;
    if (m->flags & DRM_MODE_FLAG_DBLSCAN) r /= 2.0;
    if (m->vscan > 1) r /= m->vscan;
    return r;
}


static void fill_mode(DcMode *d, const drmModeModeInfo *m)
{
    d->width = m->hdisplay;
    d->height = m->vdisplay;
    d->refresh = mode_refresh(m);
    d->interlaced = (m->flags & DRM_MODE_FLAG_INTERLACE) != 0;
    d->preferred = (m->type & DRM_MODE_TYPE_PREFERRED) != 0;
}


static int open_card(const gchar *card)
{
    gchar *path = g_strdup_printf("/dev/dri/%s", card);
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);

    if (fd < 0) {
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    }
    g_free(path);

    /*
     * The first process to open a primary node with no current master
     * becomes master implicitly.  Give it up immediately so the compositor
     * (or logind on its behalf) can always take the device.
     */
    if (fd >= 0 && drmIsMaster(fd)) {
        drmDropMaster(fd);
    }
    return fd;
}


static gchar *enum_value_name(drmModePropertyRes *p, guint64 value)
{
    gint i;

    for (i = 0; i < p->count_enums; i++) {
        if (p->enums[i].value == value) {
            return g_strdup(p->enums[i].name);
        }
    }
    return NULL;
}


static void read_connector_props(int fd, DcConnector *c, guint32 *crtc_id)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, c->connector_id,
                                   DRM_MODE_OBJECT_CONNECTOR);
    guint32 i;

    if (!props) {
        return;
    }
    for (i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        guint64 v = props->prop_values[i];

        if (!p) continue;
        if (strcmp(p->name, "CRTC_ID") == 0) {
            *crtc_id = (guint32) v;
        } else if (strcmp(p->name, "vrr_capable") == 0) {
            c->vrr_capable = v != 0;
        } else if (strcmp(p->name, "non-desktop") == 0) {
            c->non_desktop = v != 0;
        } else if (strcmp(p->name, "max bpc") == 0) {
            c->max_bpc = (gint) v;
        } else if (strcmp(p->name, "Colorspace") == 0) {
            c->colorspace = enum_value_name(p, v);
        } else if (strcmp(p->name, "Broadcast RGB") == 0) {
            c->rgb_range = enum_value_name(p, v);
        } else if (strcmp(p->name, "color format") == 0) {
            c->color_format = enum_value_name(p, v);
        } else if (strcmp(p->name, "HDR_OUTPUT_METADATA") == 0) {
            c->hdr_metadata = v != 0;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
}


static gint crtc_vrr_enabled(int fd, guint32 crtc_id)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
    gint ret = -1;
    guint32 i;

    if (!props) {
        return -1;
    }
    for (i = 0; i < props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
        if (p && strcmp(p->name, "VRR_ENABLED") == 0) {
            ret = props->prop_values[i] != 0;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return ret;
}


/* Fill in what only KMS knows: refresh rates, the current mode, VRR and
 * colour properties.  'list' holds the card's sysfs connectors. */
static void kms_update(const gchar *card, GList *list)
{
    drmModeRes *res;
    GList *l;
    int fd, i;

    fd = open_card(card);
    if (fd < 0) {
        return;
    }
    /* Atomic drivers report CRTC_ID and similar only to atomic clients */
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    res = drmModeGetResources(fd);
    if (!res) {
        close(fd);
        return;
    }

    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *k;
        DcConnector *c = NULL;
        guint32 crtc_id = 0;
        gint j;

        /* Only query the connectors asked for */
        for (l = list; l; l = l->next) {
            if (((DcConnector *) l->data)->connector_id ==
                res->connectors[i]) {
                c = l->data;
                break;
            }
        }
        if (!c) continue;

        /* "Current" does not trigger a probe of the output */
        k = drmModeGetConnectorCurrent(fd, res->connectors[i]);
        if (!k) continue;

        c->connected = (k->connection == DRM_MODE_CONNECTED);
        if (k->mmWidth && k->mmHeight) {
            c->width_mm = k->mmWidth;
            c->height_mm = k->mmHeight;
        }
        if (k->count_modes > 0) {
            g_array_set_size(c->modes, 0);
            for (j = 0; j < k->count_modes; j++) {
                DcMode m;
                fill_mode(&m, &k->modes[j]);
                g_array_append_val(c->modes, m);
            }
        }

        read_connector_props(fd, c, &crtc_id);
        if (!crtc_id && k->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(fd, k->encoder_id);
            if (enc) {
                crtc_id = enc->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        c->active = FALSE;
        if (crtc_id) {
            drmModeCrtc *crtc = drmModeGetCrtc(fd, crtc_id);
            if (crtc && crtc->mode_valid) {
                c->active = TRUE;
                fill_mode(&c->current, &crtc->mode);
                c->crtc_x = crtc->x;
                c->crtc_y = crtc->y;
                c->vrr_enabled = crtc_vrr_enabled(fd, crtc_id);
            }
            drmModeFreeCrtc(crtc);
        }
        drmModeFreeConnector(k);
    }

    drmModeFreeResources(res);
    close(fd);
}

#endif /* HAVE_LIBDRM */


static gint compare_connectors(gconstpointer a, gconstpointer b)
{
    const DcConnector *x = a, *y = b;
    return (gint) x->connector_id - (gint) y->connector_id;
}


/* The connectors of 'card', or only connector 'name' if not NULL */
static GList *kms_connectors(const gchar *card, const gchar *name)
{
    GDir *dir;
    const gchar *entry;
    gchar *prefix;
    GList *list = NULL, *l;

    if (!card || !is_card_name(card)) {
        return NULL;
    }
    prefix = g_strconcat(card, "-", NULL);
    if (name) {
        gchar *entry_name = g_strconcat(prefix, name, NULL);
        gchar *path = g_build_filename(DRM_SYSFS_DIR, entry_name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            list = g_list_prepend(list, sysfs_connector(card, entry_name));
        }
        g_free(path);
        g_free(entry_name);
    } else if ((dir = g_dir_open(DRM_SYSFS_DIR, 0, NULL))) {
        while ((entry = g_dir_read_name(dir))) {
            if (g_str_has_prefix(entry, prefix)) {
                list = g_list_prepend(list, sysfs_connector(card, entry));
            }
        }
        g_dir_close(dir);
    }
    g_free(prefix);

    list = g_list_sort(list, compare_connectors);

#ifdef HAVE_LIBDRM
    if (list && !card_runtime_suspended(card)) {
        kms_update(card, list);
    }
#endif

    for (l = list; l; l = l->next) {
        DcConnector *c = l->data;
        g_array_sort(c->modes, compare_modes);
    }
    return list;
}


GList *dc_kms_connectors(const gchar *card)
{
    return kms_connectors(card, NULL);
}


GList *dc_kms_connected(const gchar *card)
{
    GList *all = dc_kms_connectors(card), *l, *ret = NULL;

    for (l = all; l; l = l->next) {
        DcConnector *c = l->data;
        if (c->connected && !c->non_desktop) {
            ret = g_list_append(ret, c);
        } else {
            dc_connector_free(c);
        }
    }
    g_list_free(all);
    return ret;
}


DcConnector *dc_kms_connector(const gchar *card, const gchar *name)
{
    GList *one = kms_connectors(card, name);
    DcConnector *ret = one ? one->data : NULL;

    g_list_free(one);
    return ret;
}


void dc_connector_free(gpointer data)
{
    DcConnector *c = data;

    if (!c) return;
    g_free(c->card);
    g_free(c->name);
    if (c->modes) g_array_free(c->modes, TRUE);
    g_free(c->colorspace);
    g_free(c->rgb_range);
    g_free(c->color_format);
    g_free(c->edid);
    g_free(c->monitor);
    g_free(c->vendor);
    g_free(c->serial);
    g_free(c);
}


void dc_connector_list_free(GList *list)
{
    g_list_free_full(list, dc_connector_free);
}


static gint compare_strings(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const gchar * const *) a, *(const gchar * const *) b);
}


gchar *dc_connector_state_signature(void)
{
    GDir *dir = g_dir_open(DRM_SYSFS_DIR, 0, NULL);
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    GString *sig = g_string_new(NULL);
    const gchar *e;
    guint i;

    while (dir && (e = g_dir_read_name(dir))) {
        const gchar *dash = strchr(e, '-');
        gchar *card, *path, *status, *enabled;

        if (!dash) continue;
        card = g_strndup(e, dash - e);
        if (is_card_name(card)) {
            path = g_build_filename(DRM_SYSFS_DIR, e, NULL);
            status = amdgpu_read_string(path, "status");
            enabled = amdgpu_read_string(path, "enabled");
            g_ptr_array_add(items, g_strdup_printf("%s=%s/%s", e,
                                                   status ? status : "",
                                                   enabled ? enabled : ""));
            g_free(status);
            g_free(enabled);
            g_free(path);
        }
        g_free(card);
    }
    if (dir) {
        g_dir_close(dir);
    }
    /* readdir order is not stable */
    g_ptr_array_sort(items, compare_strings);
    for (i = 0; i < items->len; i++) {
        g_string_append(sig, g_ptr_array_index(items, i));
        g_string_append_c(sig, ' ');
    }
    g_ptr_array_free(items, TRUE);
    return g_string_free(sig, FALSE);
}


/*
 * The aspect ratio a resolution is sold as: 1366x768 is "16:9" and
 * 3440x1440 "21:9", not their exact reduced fractions.
 */
/* ---- EDID color formats ------------------------------------------------- */

/* Every progressive CTA-861 video format (VIC) of at least 480 lines, with
 * its exact refresh rate in mHz.  Generated from libdisplay-info 0.3.0's
 * di_cta_video_format_from_vic() (CTA-861-I); 4:2:0 data blocks refer to
 * modes by these numbers. */
static const struct {
    guint8 vic;
    guint16 width, height;
    guint32 mhz;
} cta_vics[] = {
    {   1,   640,  480,  59940 },    {   2,   720,  480,  59940 },    {   3,   720,  480,  59940 },
    {   4,  1280,  720,  60000 },    {  14,  1440,  480,  59940 },    {  15,  1440,  480,  59940 },
    {  16,  1920, 1080,  60000 },    {  17,   720,  576,  50000 },    {  18,   720,  576,  50000 },
    {  19,  1280,  720,  50000 },    {  29,  1440,  576,  50000 },    {  30,  1440,  576,  50000 },
    {  31,  1920, 1080,  50000 },    {  32,  1920, 1080,  24000 },    {  33,  1920, 1080,  25000 },
    {  34,  1920, 1080,  30000 },    {  35,  2880,  480,  59940 },    {  36,  2880,  480,  59940 },
    {  37,  2880,  576,  50000 },    {  38,  2880,  576,  50000 },    {  41,  1280,  720, 100000 },
    {  42,   720,  576, 100000 },    {  43,   720,  576, 100000 },    {  47,  1280,  720, 120000 },
    {  48,   720,  480, 119880 },    {  49,   720,  480, 119880 },    {  52,   720,  576, 200000 },
    {  53,   720,  576, 200000 },    {  56,   720,  480, 239760 },    {  57,   720,  480, 239760 },
    {  60,  1280,  720,  24000 },    {  61,  1280,  720,  25000 },    {  62,  1280,  720,  30000 },
    {  63,  1920, 1080, 120000 },    {  64,  1920, 1080, 100000 },    {  65,  1280,  720,  24000 },
    {  66,  1280,  720,  25000 },    {  67,  1280,  720,  30000 },    {  68,  1280,  720,  50000 },
    {  69,  1280,  720,  60000 },    {  70,  1280,  720, 100000 },    {  71,  1280,  720, 120000 },
    {  72,  1920, 1080,  24000 },    {  73,  1920, 1080,  25000 },    {  74,  1920, 1080,  30000 },
    {  75,  1920, 1080,  50000 },    {  76,  1920, 1080,  60000 },    {  77,  1920, 1080, 100000 },
    {  78,  1920, 1080, 120000 },    {  79,  1680,  720,  24000 },    {  80,  1680,  720,  25000 },
    {  81,  1680,  720,  30000 },    {  82,  1680,  720,  50000 },    {  83,  1680,  720,  60000 },
    {  84,  1680,  720, 100000 },    {  85,  1680,  720, 120000 },    {  86,  2560, 1080,  24000 },
    {  87,  2560, 1080,  25000 },    {  88,  2560, 1080,  30000 },    {  89,  2560, 1080,  50000 },
    {  90,  2560, 1080,  60000 },    {  91,  2560, 1080, 100000 },    {  92,  2560, 1080, 120000 },
    {  93,  3840, 2160,  24000 },    {  94,  3840, 2160,  25000 },    {  95,  3840, 2160,  30000 },
    {  96,  3840, 2160,  50000 },    {  97,  3840, 2160,  60000 },    {  98,  4096, 2160,  24000 },
    {  99,  4096, 2160,  25000 },    { 100,  4096, 2160,  30000 },    { 101,  4096, 2160,  50000 },
    { 102,  4096, 2160,  60000 },    { 103,  3840, 2160,  24000 },    { 104,  3840, 2160,  25000 },
    { 105,  3840, 2160,  30000 },    { 106,  3840, 2160,  50000 },    { 107,  3840, 2160,  60000 },
    { 108,  1280,  720,  48000 },    { 109,  1280,  720,  48000 },    { 110,  1680,  720,  48000 },
    { 111,  1920, 1080,  48000 },    { 112,  1920, 1080,  48000 },    { 113,  2560, 1080,  48000 },
    { 114,  3840, 2160,  48000 },    { 115,  4096, 2160,  48000 },    { 116,  3840, 2160,  48000 },
    { 117,  3840, 2160, 100000 },    { 118,  3840, 2160, 120000 },    { 119,  3840, 2160, 100000 },
    { 120,  3840, 2160, 120000 },    { 121,  5120, 2160,  24000 },    { 122,  5120, 2160,  25000 },
    { 123,  5120, 2160,  30000 },    { 124,  5120, 2160,  48000 },    { 125,  5120, 2160,  50000 },
    { 126,  5120, 2160,  60000 },    { 127,  5120, 2160, 100000 },    { 193,  5120, 2160, 120000 },
    { 194,  7680, 4320,  24000 },    { 195,  7680, 4320,  25000 },    { 196,  7680, 4320,  30000 },
    { 197,  7680, 4320,  48000 },    { 198,  7680, 4320,  50000 },    { 199,  7680, 4320,  60000 },
    { 200,  7680, 4320, 100000 },    { 201,  7680, 4320, 120000 },    { 202,  7680, 4320,  24000 },
    { 203,  7680, 4320,  25000 },    { 204,  7680, 4320,  30000 },    { 205,  7680, 4320,  48000 },
    { 206,  7680, 4320,  50000 },    { 207,  7680, 4320,  60000 },    { 208,  7680, 4320, 100000 },
    { 209,  7680, 4320, 120000 },    { 210, 10240, 4320,  24000 },    { 211, 10240, 4320,  25000 },
    { 212, 10240, 4320,  30000 },    { 213, 10240, 4320,  48000 },    { 214, 10240, 4320,  50000 },
    { 215, 10240, 4320,  60000 },    { 216, 10240, 4320, 100000 },    { 217, 10240, 4320, 120000 },
    { 218,  4096, 2160, 100000 },    { 219,  4096, 2160, 120000 },
};


static gboolean vic_matches_mode(guint vic, const DcMode *mode)
{
    guint i;

    if (!mode || mode->interlaced) return FALSE;
    for (i = 0; i < G_N_ELEMENTS(cta_vics); i++) {
        gdouble hz;

        if (cta_vics[i].vic != vic) continue;
        if (mode->width != cta_vics[i].width ||
            mode->height != cta_vics[i].height) {
            return FALSE;
        }
        /* A VIC covers both its integer rate and the 1000/1001 one */
        hz = cta_vics[i].mhz / 1000.0;
        return fabs(mode->refresh - hz) < 0.5 ||
               fabs(mode->refresh - hz * 1000.0 / 1001.0) < 0.5 ||
               fabs(mode->refresh - hz * 1001.0 / 1000.0) < 0.5;
    }
    return FALSE;
}


/* What the CTA-861 data blocks of an EDID say about YCbCr 4:2:0, gathered
 * as the kernel does (drm_edid.c): the SVDs of all video data blocks in
 * order, skipping invalid VICs, and the capability maps over the first 64 */
typedef struct {
    guint8 svds[64];
    guint n_svds;
    guint64 cmdb_map;
    gboolean y420;           /* the mode can be sent as YCbCr 4:2:0 */
    const DcMode *mode;
} CtaState;


static gboolean valid_vic(guint vic)
{
    return (vic >= 1 && vic <= 127) || (vic >= 193 && vic <= 219);
}


/* A collection of CTA-861 data blocks: from a CTA extension, or embedded in
 * a DisplayID CTA data block */
static void parse_cta_data_blocks(const guchar *p, guint len, CtaState *st)
{
    guint pos = 0;

    while (pos < len) {
        guint tag = p[pos] >> 5, l = p[pos] & 31, i;
        const guchar *data = p + pos + 1;

        if (pos + 1 + l > len) break;
        if (tag == 2) {                           /* video data block */
            for (i = 0; i < l; i++) {
                guint v = data[i];
                if (v >= 129 && v <= 192) v &= 0x7f;   /* native flag */
                if (valid_vic(v) && st->n_svds < G_N_ELEMENTS(st->svds)) {
                    st->svds[st->n_svds++] = v;
                }
            }
        } else if (tag == 7 && l >= 1 && data[0] == 0x0e) {
            /* 4:2:0 video data block: modes only sent as 4:2:0 */
            for (i = 1; i < l; i++) {
                st->y420 |= vic_matches_mode(data[i], st->mode);
            }
        } else if (tag == 7 && l >= 1 && data[0] == 0x0f) {
            /* 4:2:0 capability map over the SVDs; empty means all */
            if (l == 1) {
                st->cmdb_map = G_MAXUINT64;
            }
            for (i = 1; i < l && i <= 8; i++) {
                st->cmdb_map |= (guint64) data[i] << (8 * (i - 1));
            }
        }
        pos += 1 + l;
    }
}


/* The CTA data blocks embedded in a DisplayID extension block (0x70) */
static void parse_displayid(const guchar *x, CtaState *st)
{
    guint end = MIN(5u + x[2], 127u), pos = 5;

    while (pos + 3 <= end) {
        guint tag = x[pos], l = x[pos + 2];

        if (pos + 3 + l > end) break;
        if (tag == 0x81) {                        /* CTA DisplayID block */
            parse_cta_data_blocks(x + pos + 3, l, st);
        }
        pos += 3 + l;
    }
}


guint dc_edid_color_formats(const guchar *edid, gsize len,
                            const DcMode *mode)
{
    guint mask = (1u << DC_COLOR_FORMAT_AUTO) | (1u << DC_COLOR_FORMAT_RGB);
    CtaState st = { .mode = mode };
    guint blocks, b, i;

    if (!edid || len < 128) {
        return mask | (1u << DC_COLOR_FORMAT_YCBCR444) |
               (1u << DC_COLOR_FORMAT_YCBCR422) |
               (1u << DC_COLOR_FORMAT_YCBCR420);
    }

    /* EDID 1.4 digital displays declare YCbCr 4:4:4 / 4:2:2 in the base
     * block, CTA-861 extensions in their header */
    if ((edid[20] & 0x80) && edid[18] == 1 && edid[19] >= 4) {
        guint enc = (edid[24] >> 3) & 3;
        if (enc & 1) mask |= 1u << DC_COLOR_FORMAT_YCBCR444;
        if (enc & 2) mask |= 1u << DC_COLOR_FORMAT_YCBCR422;
    }

    blocks = MIN(edid[126], len / 128 - 1);
    for (b = 1; b <= blocks; b++) {
        const guchar *x = edid + 128 * b;

        if (x[0] == 0x02) {                       /* CTA-861 extension */
            if (x[3] & 0x20) mask |= 1u << DC_COLOR_FORMAT_YCBCR444;
            if (x[3] & 0x10) mask |= 1u << DC_COLOR_FORMAT_YCBCR422;
            if (x[2] >= 4) {
                parse_cta_data_blocks(x + 4, MIN(x[2], 127u) - 4, &st);
            }
        } else if (x[0] == 0x70) {                /* DisplayID extension */
            parse_displayid(x, &st);
        }
    }

    /*
     * 4:2:0 is defined for progressive modes only, so vic_matches_mode()
     * never matches an interlaced one.
     */
    for (i = 0; i < st.n_svds && i < 64; i++) {
        if ((st.cmdb_map & ((guint64) 1 << i)) &&
            vic_matches_mode(st.svds[i], mode)) {
            st.y420 = TRUE;
        }
    }
    if (st.y420) {
        mask |= 1u << DC_COLOR_FORMAT_YCBCR420;
    }
    return mask;
}


gchar *dc_aspect_ratio(guint width, guint height)
{
    static const struct { const gchar *name; gdouble lo, hi; } names[] = {
        { "1:1",   1.00,  1.00  },
        { "5:4",   1.25,  1.25  },
        { "4:3",   1.333, 1.334 },
        { "3:2",   1.50,  1.50  },
        { "16:10", 1.60,  1.60  },
        { "5:3",   1.666, 1.667 },
        { "16:9",  1.77,  1.79  },   /* incl. 1360x768, 1366x768 */
        { "17:9",  1.89,  1.90  },   /* DCI 4K: 4096x2160 */
        { "2:1",   2.00,  2.00  },
        { "21:9",  2.33,  2.40  },   /* 2560x1080, 3440x1440, 3840x1600 */
        { "32:10", 3.20,  3.20  },
        { "32:9",  3.55,  3.56  },
    };
    gdouble r;
    guint a, b, t, i;

    if (!width || !height) {
        return NULL;
    }
    r = (gdouble) width / height;
    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        if (r >= names[i].lo - 0.005 && r <= names[i].hi + 0.005) {
            return g_strdup(names[i].name);
        }
    }

    /* Anything else: the reduced fraction if it is short, else x:1 */
    for (a = width, b = height; b; t = a % b, a = b, b = t);
    if (width / a <= 32 && height / a <= 32) {
        return g_strdup_printf("%u:%u", width / a, height / a);
    }
    return g_strdup_printf("%.2f:1", r);
}


/* "1920x1080 (16:9)" */
gchar *dc_resolution_string(guint width, guint height, gboolean interlaced)
{
    gchar *aspect = dc_aspect_ratio(width, height);
    gchar *s = aspect
        ? g_strdup_printf("%ux%u%s (%s)", width, height,
                          interlaced ? "i" : "", aspect)
        : g_strdup_printf("%ux%u%s", width, height, interlaced ? "i" : "");

    g_free(aspect);
    return s;
}


gchar *dc_mode_string(const DcMode *m)
{
    gchar *res = dc_resolution_string(m->width, m->height, m->interlaced);
    gchar *s;

    if (m->refresh <= 0) {
        return res;
    }
    s = g_strdup_printf("%s @ %.2f Hz", res, m->refresh);
    g_free(res);
    return s;
}


gchar *dc_connector_title(const DcConnector *c)
{
    return g_strdup_printf("%s - (%s)", c->name,
                           c->monitor ? c->monitor : "Unknown Display");
}


/* ---- hotplug ------------------------------------------------------------ */

typedef struct {
    GSourceFunc func;
    gpointer data;
    guint timeout;
} HotplugWatch;


static gboolean hotplug_debounced(gpointer user_data)
{
    HotplugWatch *w = user_data;

    w->timeout = 0;
    w->func(w->data);
    return FALSE;
}


static gboolean hotplug_readable(GIOChannel *ch, GIOCondition cond,
                                 gpointer user_data)
{
    HotplugWatch *w = user_data;
    gchar buf[4096];
    gssize n;
    gboolean drm = FALSE;

    if (cond & (G_IO_ERR | G_IO_HUP)) {
        return FALSE;
    }
    n = recv(g_io_channel_unix_get_fd(ch), buf, sizeof(buf) - 1,
             MSG_DONTWAIT);
    if (n > 0) {
        gssize off = 0;
        buf[n] = '\0';
        /* "change@/devices/.../drm/card1\0ACTION=change\0SUBSYSTEM=drm..." */
        while (off < n) {
            const gchar *kv = buf + off;
            if (strcmp(kv, "SUBSYSTEM=drm") == 0) {
                drm = TRUE;
            }
            off += strlen(kv) + 1;
        }
    }
    if (drm) {
        if (w->timeout) {
            g_source_remove(w->timeout);
        }
        w->timeout = g_timeout_add(HOTPLUG_DEBOUNCE_MS, hotplug_debounced, w);
    }
    return TRUE;
}


gboolean dc_watch_hotplug(GSourceFunc func, gpointer data)
{
    struct sockaddr_nl addr;
    HotplugWatch *w;
    GIOChannel *ch;
    int fd;

    fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        return FALSE;
    }
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 1;   /* kernel uevents */
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return FALSE;
    }

    w = g_new0(HotplugWatch, 1);
    w->func = func;
    w->data = data;
    ch = g_io_channel_unix_new(fd);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN | G_IO_ERR | G_IO_HUP, hotplug_readable, w);
    g_io_channel_unref(ch);
    return TRUE;
}


/* ---- desktop display settings ------------------------------------------- */

static const struct {
    const gchar *desktop;   /* prefix of DcCompositor::desktop */
    const gchar *name;
    const gchar *argv[4];
} settings_tools[] = {
    { "GNOME",      "GNOME Settings",
      { "gnome-control-center", "display", NULL } },
    { "KDE Plasma", "System Settings",
      { "systemsettings", "kcm_kscreen", NULL } },
    { "KDE Plasma", "System Settings",
      { "kcmshell6", "kcm_kscreen", NULL } },
    { "KDE Plasma", "System Settings",
      { "kcmshell5", "kcm_kscreen", NULL } },
    { "Cinnamon",   "Cinnamon Display Settings",
      { "cinnamon-settings", "display", NULL } },
    { "COSMIC",     "COSMIC Settings",
      { "cosmic-settings", "displays", NULL } },
    /* wlroots compositors have no settings application of their own */
    { "",           "nwg-displays", { "nwg-displays", NULL } },
    { "",           "wdisplays",    { "wdisplays", NULL } },
    { NULL, NULL, { NULL } }
};


static gint find_settings_tool(void)
{
    const DcCompositor *comp = dc_compositor();
    const gchar *desktop = comp ? comp->desktop : NULL;
    gchar *env = NULL;
    gint i, ret = -1;

    if (!desktop) {
        desktop = env = desktop_from_env();
    }
    for (i = 0; settings_tools[i].name; i++) {
        gchar *path;

        if (settings_tools[i].desktop[0] &&
            (!desktop ||
             !g_str_has_prefix(desktop, settings_tools[i].desktop))) {
            continue;
        }
        path = g_find_program_in_path(settings_tools[i].argv[0]);
        g_free(path);
        if (path) {
            ret = i;
            break;
        }
    }
    g_free(env);
    return ret;
}


const gchar *dc_desktop_display_settings_name(void)
{
    gint i = find_settings_tool();
    return i >= 0 ? settings_tools[i].name : NULL;
}


gboolean dc_open_desktop_display_settings(GError **error)
{
    gint i = find_settings_tool();

    if (i < 0) {
        g_set_error_literal(error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT,
                            "No display settings tool was found for this "
                            "desktop.");
        return FALSE;
    }
    return g_spawn_async(NULL, (gchar **) settings_tools[i].argv, NULL,
                         G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, error);
}
