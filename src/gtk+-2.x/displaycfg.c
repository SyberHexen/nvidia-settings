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


gchar *dc_mode_string(const DcMode *m)
{
    if (m->refresh > 0) {
        return g_strdup_printf("%ux%u%s @ %.2f Hz", m->width, m->height,
                               m->interlaced ? "i" : "", m->refresh);
    }
    return g_strdup_printf("%ux%u%s", m->width, m->height,
                           m->interlaced ? "i" : "");
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
