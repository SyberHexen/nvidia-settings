/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * Display configuration backends: selection, change notification and the
 * helpers shared by all of them.
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

#include <glib.h>

#include "displaycfg.h"

#define NOTIFY_DEBOUNCE_MS 300

typedef struct {
    GSourceFunc func;
    gpointer data;
} DcWatch;


/* ---- selection ---------------------------------------------------------- */

static DcBackend *mutter(void)
{
    return dc_backend_mutter_new("org.gnome.Mutter.DisplayConfig",
                                 "/org/gnome/Mutter/DisplayConfig",
                                 "org.gnome.Mutter.DisplayConfig");
}


static DcBackend *muffin(void)
{
    return dc_backend_mutter_new("org.cinnamon.Muffin.DisplayConfig",
                                 "/org/cinnamon/Muffin/DisplayConfig",
                                 "org.cinnamon.Muffin.DisplayConfig");
}


DcBackend *dc_backend(void)
{
    static gboolean done = FALSE;
    static DcBackend *backend = NULL;
    const DcCompositor *comp;
    const gchar *proc;
    DcBackend *(*order[6])(void);
    gint n = 0, i;

    if (done) {
        return backend;
    }
    done = TRUE;

    if (dc_session_type() != DC_SESSION_WAYLAND) {
        return NULL;
    }
    comp = dc_compositor();
    proc = comp && comp->process ? comp->process : "";

    /* Try the compositor's own interface first, then probe the rest */
    if (!strcmp(proc, "gnome-shell") || !strcmp(proc, "mutter")) {
        order[n++] = mutter;
    } else if (!strcmp(proc, "cinnamon") || !strcmp(proc, "muffin")) {
        order[n++] = muffin;
    } else if (!strcmp(proc, "kwin_wayland")) {
        order[n++] = dc_backend_kde_new;
    }
    order[n++] = dc_backend_wlr_new;
    order[n++] = dc_backend_kde_new;
    order[n++] = mutter;
    order[n++] = muffin;

    for (i = 0; i < n && !backend; i++) {
        backend = order[i]();
    }
    return backend;
}


/* ---- change notification ------------------------------------------------ */

static gboolean notify_idle(gpointer data)
{
    DcBackend *b = data;
    GList *l, *copy;

    b->notify_id = 0;
    /* A callback may remove its own watch */
    copy = g_list_copy(b->watches);
    for (l = copy; l; l = l->next) {
        if (g_list_find(b->watches, l->data)) {
            DcWatch *w = l->data;
            w->func(w->data);
        }
    }
    g_list_free(copy);
    return FALSE;
}


static void invalidate(DcBackend *b)
{
    dc_output_list_free(b->cache);
    b->cache = NULL;
    b->cache_valid = FALSE;
}


GList *dc_backend_outputs(DcBackend *b, GError **error)
{
    if (!b->cache_valid) {
        GError *e = NULL;
        GList *outputs = b->query(b, &e);
        if (e) {
            g_propagate_error(error, e);
            dc_output_list_free(outputs);
            return NULL;
        }
        b->cache = outputs;
        b->cache_valid = TRUE;
    }
    return dc_output_list_copy(b->cache);
}


gboolean dc_backend_apply(DcBackend *b, GList *outputs, gboolean persistent,
                          GError **error)
{
    gboolean ok = b->apply(b, outputs, persistent, error);
    invalidate(b);
    return ok;
}


void dc_backend_notify_changed(DcBackend *b)
{
    invalidate(b);
    if (b->notify_id) {
        g_source_remove(b->notify_id);
    }
    b->notify_id = g_timeout_add(NOTIFY_DEBOUNCE_MS, notify_idle, b);
}


void dc_backend_add_watch(DcBackend *b, GSourceFunc func, gpointer data)
{
    DcWatch *w = g_new0(DcWatch, 1);

    w->func = func;
    w->data = data;
    b->watches = g_list_append(b->watches, w);
}


void dc_backend_remove_watch(DcBackend *b, GSourceFunc func, gpointer data)
{
    GList *l;

    for (l = b->watches; l; l = l->next) {
        DcWatch *w = l->data;
        if (w->func == func && w->data == data) {
            b->watches = g_list_delete_link(b->watches, l);
            g_free(w);
            return;
        }
    }
}


/* ---- outputs ------------------------------------------------------------ */

DcOutput *dc_output_copy(const DcOutput *o)
{
    DcOutput *c = g_new(DcOutput, 1);
    guint i;

    *c = *o;
    c->name = g_strdup(o->name);
    c->make = g_strdup(o->make);
    c->model = g_strdup(o->model);
    c->serial = g_strdup(o->serial);
    c->description = g_strdup(o->description);
    c->modes = g_array_sized_new(FALSE, FALSE, sizeof(DcOutputMode),
                                 o->modes->len);
    for (i = 0; i < o->modes->len; i++) {
        DcOutputMode m = g_array_index(o->modes, DcOutputMode, i);
        m.id = g_strdup(m.id);
        if (m.scales) {
            GArray *s = g_array_sized_new(FALSE, FALSE, sizeof(gdouble),
                                          m.scales->len);
            g_array_append_vals(s, m.scales->data, m.scales->len);
            m.scales = s;
        }
        g_array_append_val(c->modes, m);
    }
    return c;
}


void dc_output_free(gpointer data)
{
    DcOutput *o = data;
    guint i;

    if (!o) return;
    for (i = 0; o->modes && i < o->modes->len; i++) {
        DcOutputMode *m = &g_array_index(o->modes, DcOutputMode, i);
        g_free(m->id);
        if (m->scales) g_array_free(m->scales, TRUE);
    }
    if (o->modes) g_array_free(o->modes, TRUE);
    g_free(o->name);
    g_free(o->make);
    g_free(o->model);
    g_free(o->serial);
    g_free(o->description);
    g_free(o);
}


void dc_output_list_free(GList *list)
{
    g_list_free_full(list, dc_output_free);
}


GList *dc_output_list_copy(GList *list)
{
    GList *ret = NULL;

    for (; list; list = list->next) {
        ret = g_list_append(ret, dc_output_copy(list->data));
    }
    return ret;
}


void dc_output_extent(const DcBackend *b, const DcOutput *o, gint *w, gint *h)
{
    const DcOutputMode *m;
    gdouble mw, mh;

    *w = *h = 0;
    if (!o->enabled || o->current_mode < 0 ||
        o->current_mode >= (gint) o->modes->len) {
        return;
    }
    m = &g_array_index(o->modes, DcOutputMode, o->current_mode);
    mw = m->width;
    mh = m->height;
    if (o->transform & 1) {     /* 90 and 270 degrees */
        gdouble t = mw;
        mw = mh;
        mh = t;
    }
    if ((b->caps & DC_CAP_LOGICAL_LAYOUT) && o->scale > 0) {
        mw /= o->scale;
        mh /= o->scale;
    }
    *w = (gint) floor(mw + 0.5);
    *h = (gint) floor(mh + 0.5);
}


/* Mutter calls HDMI-A connectors "HDMI" */
static gchar *normalize_name(const gchar *name)
{
    if (g_str_has_prefix(name, "HDMI-A-")) {
        return g_strconcat("HDMI-", name + strlen("HDMI-A-"), NULL);
    }
    return g_strdup(name);
}


static gboolean same_serial(const gchar *a, const gchar *b)
{
    gchar *end_a, *end_b;
    guint64 x, y;

    if (!a || !b || !*a || !*b) {
        return FALSE;
    }
    if (strcmp(a, b) == 0) {
        return TRUE;
    }
    /* "0x616d0000" (Mutter) and "1634533376" are the same EDID serial */
    x = g_ascii_strtoull(a, &end_a, 0);
    y = g_ascii_strtoull(b, &end_b, 0);
    return *end_a == '\0' && *end_b == '\0' && x == y && x != 0;
}


static gboolean edid_matches(const DcOutput *o, const DcConnector *c)
{
    if (same_serial(o->serial, c->serial)) {
        return TRUE;
    }
    return c->monitor && o->model &&
           g_ascii_strcasecmp(c->monitor, o->model) == 0;
}


DcOutput *dc_output_for_connector(GList *outputs, const DcConnector *c,
                                  gboolean unique_name)
{
    gchar *want = normalize_name(c->name);
    DcOutput *by_name = NULL, *by_edid = NULL;
    gint name_hits = 0, edid_hits = 0;
    GList *l;

    for (l = outputs; l; l = l->next) {
        DcOutput *o = l->data;
        gchar *have = normalize_name(o->name);
        gboolean name = strcmp(have, want) == 0;
        gboolean edid = edid_matches(o, c);

        g_free(have);
        if (name && (edid || unique_name)) {
            by_name = o;
            name_hits++;
        }
        if (edid) {
            by_edid = o;
            edid_hits++;
        }
    }
    g_free(want);

    if (name_hits == 1) {
        return by_name;
    }
    /* The compositor names connectors differently; fall back to EDID */
    if (edid_hits == 1) {
        return by_edid;
    }
    return NULL;
}


void dc_layout_fixup(const DcBackend *b, GList *outputs, DcOutput *changed,
                     const DcOutput *before, gboolean moved)
{
    gint old_w, old_h, new_w, new_h, dx, dy;
    gint min_x = G_MAXINT, min_y = G_MAXINT;
    GList *l;

    dc_output_extent(b, before, &old_w, &old_h);
    dc_output_extent(b, changed, &new_w, &new_h);

    if (!before->enabled && changed->enabled && !moved) {
        /* Turned on: append it to the right of the layout */
        gint right = 0, top = G_MAXINT;
        for (l = outputs; l; l = l->next) {
            DcOutput *o = l->data;
            gint w, h;
            if (o == changed || !o->enabled) continue;
            dc_output_extent(b, o, &w, &h);
            right = MAX(right, o->x + w);
            top = MIN(top, o->y);
        }
        changed->x = right;
        changed->y = top == G_MAXINT ? 0 : top;
    } else if (before->enabled && !moved) {
        /* Resized or turned off: neighbours follow its far edges */
        dx = new_w - old_w;
        dy = new_h - old_h;
        for (l = outputs; l; l = l->next) {
            DcOutput *o = l->data;
            if (o == changed || !o->enabled) continue;
            if (dx && o->x >= before->x + old_w) {
                o->x += dx;
            }
            if (dy && o->y >= before->y + old_h) {
                o->y += dy;
            }
        }
    }
    for (l = outputs; l; l = l->next) {
        DcOutput *o = l->data;
        if (!o->enabled) continue;
        min_x = MIN(min_x, o->x);
        min_y = MIN(min_y, o->y);
    }
    if (min_x == G_MAXINT) {
        return;
    }
    for (l = outputs; l; l = l->next) {
        DcOutput *o = l->data;
        if (!o->enabled) continue;
        o->x -= min_x;
        o->y -= min_y;
    }
}


const gchar *dc_transform_name(DcTransform t)
{
    static const gchar *names[] = {
        "Normal", "Left", "Inverted", "Right",
        "Flipped", "Flipped Left", "Flipped Inverted", "Flipped Right",
    };
    return (t >= 0 && t < 8) ? names[t] : "Unknown";
}
