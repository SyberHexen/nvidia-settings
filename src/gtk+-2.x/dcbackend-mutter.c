/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * Display configuration backend for GNOME (Mutter) and Cinnamon (Muffin),
 * through their DisplayConfig D-Bus interface.  Both export the same
 * GetCurrentState / ApplyMonitorsConfig methods under different names.
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

#include <string.h>
#include <math.h>

#include <gio/gio.h>

#include "displaycfg.h"

#define CALL_TIMEOUT_MS 5000

/* ApplyMonitorsConfig methods */
#define METHOD_VERIFY     0
#define METHOD_TEMPORARY  1
#define METHOD_PERSISTENT 2

/* layout-mode property */
#define LAYOUT_LOGICAL    1
#define LAYOUT_PHYSICAL   2

typedef struct {
    GDBusConnection *bus;
    gchar *bus_name;
    gchar *path;
    gchar *iface;
    guint32 serial;
    guint signal_id;
} MutterPriv;


static GVariant *call(MutterPriv *p, const gchar *method, GVariant *args,
                      const GVariantType *reply, GError **error)
{
    return g_dbus_connection_call_sync(p->bus, p->bus_name, p->path, p->iface,
                                       method, args, reply,
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                       CALL_TIMEOUT_MS, NULL, error);
}


static gboolean lookup_bool(GVariant *dict, const gchar *key)
{
    gboolean v = FALSE;
    g_variant_lookup(dict, key, "b", &v);
    return v;
}


/* A setting offered as "<key>" (u) plus "<supported_key>" (au); Muffin
 * (Cinnamon) lists the RGB range and color format this way.  Returns the
 * current value, 0 if the setting is absent. */
static gint lookup_choice(GVariant *dict, const gchar *key,
                          const gchar *supported_key, guint *mask)
{
    guint32 value = 0, v;
    GVariantIter *it;

    *mask = 0;
    if (!g_variant_lookup(dict, key, "u", &value) ||
        !g_variant_lookup(dict, supported_key, "au", &it)) {
        return 0;
    }
    while (g_variant_iter_next(it, "u", &v)) {
        if (v < 32) {
            *mask |= 1u << v;
        }
    }
    g_variant_iter_free(it);
    return *mask ? (gint) value : 0;
}


/* ---- query -------------------------------------------------------------- */

static void parse_mode(GVariant *mv, DcOutputMode *m, gboolean *current)
{
    const gchar *id, *rate_mode = NULL;
    gint32 w, h;
    gdouble refresh, preferred_scale;
    GVariantIter *scales;
    GVariant *props;
    gdouble s;

    g_variant_get(mv, "(&siidd@ad@a{sv})", &id, &w, &h, &refresh,
                  &preferred_scale, NULL, &props);
    memset(m, 0, sizeof(*m));
    m->id = g_strdup(id);
    m->width = w;
    m->height = h;
    m->refresh = refresh;
    m->preferred = lookup_bool(props, "is-preferred");
    m->interlaced = lookup_bool(props, "is-interlaced");
    if (g_variant_lookup(props, "refresh-rate-mode", "&s", &rate_mode)) {
        m->variable = g_strcmp0(rate_mode, "variable") == 0;
    }
    *current = lookup_bool(props, "is-current");

    m->scales = g_array_new(FALSE, FALSE, sizeof(gdouble));
    g_variant_get_child(mv, 5, "ad", &scales);
    while (g_variant_iter_next(scales, "d", &s)) {
        g_array_append_val(m->scales, s);
    }
    g_variant_iter_free(scales);
    g_variant_unref(props);
}


static GList *mutter_query(DcBackend *b, GError **error)
{
    MutterPriv *p = b->priv;
    GVariant *reply, *monitors, *logical, *props, *v;
    GVariantIter it;
    GList *outputs = NULL, *l;
    guint32 layout = LAYOUT_LOGICAL;

    reply = call(p, "GetCurrentState", NULL,
                 G_VARIANT_TYPE("(ua((ssss)a(siiddada{sv})a{sv})"
                                "a(iiduba(ssss)a{sv})a{sv})"), error);
    if (!reply) {
        return NULL;
    }
    g_variant_get(reply, "(u@a((ssss)a(siiddada{sv})a{sv})"
                         "@a(iiduba(ssss)a{sv})@a{sv})",
                  &p->serial, &monitors, &logical, &props);

    /* A persistent change makes GNOME Shell / Cinnamon ask the user to keep
     * it and revert it otherwise, like their own display settings do. */
    b->caps = DC_CAP_PRIMARY | DC_CAP_PERSIST | DC_CAP_DISABLE |
              DC_CAP_COMPOSITOR_CONFIRMS;
    g_variant_lookup(props, "layout-mode", "u", &layout);
    if (layout == LAYOUT_LOGICAL) {
        b->caps |= DC_CAP_LOGICAL_LAYOUT;
    }
    if (lookup_bool(props, "global-scale-required")) {
        b->caps |= DC_CAP_GLOBAL_SCALE;
    }

    /* physical monitors */
    g_variant_iter_init(&it, monitors);
    while ((v = g_variant_iter_next_value(&it))) {
        const gchar *conn, *vendor, *product, *serial, *name = NULL;
        GVariant *modes, *mprops, *mv;
        GVariantIter mit;
        DcOutput *o = g_new0(DcOutput, 1);
        gboolean have_variable = FALSE;

        g_variant_get(v, "((&s&s&s&s)@a(siiddada{sv})@a{sv})",
                      &conn, &vendor, &product, &serial, &modes, &mprops);
        o->name = g_strdup(conn);
        o->make = g_strdup(vendor);
        o->model = g_strdup(product);
        o->serial = g_strdup(serial);
        if (g_variant_lookup(mprops, "display-name", "&s", &name)) {
            o->description = g_strdup(name);
        }
        o->scale = 1.0;
        o->current_mode = -1;
        o->vrr = DC_VRR_UNSUPPORTED;
        o->rgb_range = lookup_choice(mprops, "rgb-range",
                                     "supported-rgb-ranges", &o->rgb_ranges);
        o->color_format = lookup_choice(mprops, "color-format",
                                        "supported-color-formats",
                                        &o->color_formats);
        o->modes = g_array_new(FALSE, FALSE, sizeof(DcOutputMode));

        g_variant_iter_init(&mit, modes);
        while ((mv = g_variant_iter_next_value(&mit))) {
            DcOutputMode m;
            gboolean current;

            parse_mode(mv, &m, &current);
            have_variable |= m.variable;
            g_array_append_val(o->modes, m);
            if (current) {
                o->current_mode = o->modes->len - 1;
            }
            g_variant_unref(mv);
        }
        /* GNOME 46+: VRR is offered as a "variable" twin of each mode */
        if (have_variable) {
            o->vrr_policies = (1 << DC_VRR_NEVER) | (1 << DC_VRR_ALWAYS);
            o->vrr = (o->current_mode >= 0 &&
                      g_array_index(o->modes, DcOutputMode,
                                    o->current_mode).variable)
                     ? DC_VRR_ALWAYS : DC_VRR_NEVER;
        }

        outputs = g_list_append(outputs, o);
        g_variant_unref(modes);
        g_variant_unref(mprops);
        g_variant_unref(v);
    }

    /* logical monitors: position, scale, transform, primary */
    g_variant_iter_init(&it, logical);
    while ((v = g_variant_iter_next_value(&it))) {
        gint32 x, y;
        gdouble scale;
        guint32 transform;
        gboolean primary;
        GVariantIter *mons;
        const gchar *conn;

        g_variant_get(v, "(iidub@a(ssss)@a{sv})", &x, &y, &scale, &transform,
                      &primary, NULL, NULL);
        g_variant_get_child(v, 5, "a(ssss)", &mons);
        while (g_variant_iter_next(mons, "(&s&s&s&s)", &conn, NULL, NULL,
                                   NULL)) {
            for (l = outputs; l; l = l->next) {
                DcOutput *o = l->data;
                if (strcmp(o->name, conn) == 0) {
                    o->enabled = TRUE;
                    o->x = x;
                    o->y = y;
                    o->scale = scale;
                    o->transform = transform & 7;
                    o->primary = primary;
                }
            }
        }
        g_variant_iter_free(mons);
        g_variant_unref(v);
    }

    g_variant_unref(monitors);
    g_variant_unref(logical);
    g_variant_unref(props);
    g_variant_unref(reply);
    return outputs;
}


/* ---- apply -------------------------------------------------------------- */

typedef struct {
    gint x, y;
    gdouble scale;
    guint transform;
    gboolean primary;
    GList *outputs;   /* DcOutput*, not owned */
} LogicalMonitor;


/* Primary first, then top-to-bottom, left-to-right.  Muffin numbers the
 * monitors in the order they are applied, and Cinnamon pins its panels to
 * monitor numbers, so the primary monitor must stay monitor 0. */
static gint compare_logical(gconstpointer a, gconstpointer b)
{
    const LogicalMonitor *x = a, *y = b;

    if (x->primary != y->primary) {
        return x->primary ? -1 : 1;
    }
    if (x->y != y->y) {
        return x->y < y->y ? -1 : 1;
    }
    return x->x < y->x ? -1 : x->x > y->x;
}


static gboolean mutter_apply(DcBackend *b, GList *outputs,
                             gboolean persistent, GError **error)
{
    MutterPriv *p = b->priv;
    GList *groups = NULL, *l, *g;
    GVariantBuilder lm;
    GVariant *reply;
    gboolean have_primary = FALSE;
    GList *fresh;

    /* ApplyMonitorsConfig needs the serial of the current state */
    fresh = mutter_query(b, error);
    if (!fresh && error && *error) {
        return FALSE;
    }
    dc_output_list_free(fresh);

    /* Mirrored monitors share one logical monitor (same position) */
    for (l = outputs; l; l = l->next) {
        DcOutput *o = l->data;
        LogicalMonitor *lmon = NULL;

        if (!o->enabled || o->current_mode < 0) continue;
        for (g = groups; g; g = g->next) {
            LogicalMonitor *c = g->data;
            if (c->x == o->x && c->y == o->y) {
                lmon = c;
                break;
            }
        }
        if (!lmon) {
            lmon = g_new0(LogicalMonitor, 1);
            lmon->x = o->x;
            lmon->y = o->y;
            lmon->scale = o->scale;
            lmon->transform = o->transform;
            groups = g_list_append(groups, lmon);
        }
        lmon->primary |= o->primary;
        lmon->outputs = g_list_append(lmon->outputs, o);
    }

    if (!groups) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "At least one display must stay enabled.");
        return FALSE;
    }

    /* Exactly one logical monitor is primary */
    for (g = groups; g; g = g->next) {
        LogicalMonitor *c = g->data;
        if (c->primary && !have_primary) {
            have_primary = TRUE;
        } else {
            c->primary = FALSE;
        }
    }
    if (!have_primary) {
        ((LogicalMonitor *) groups->data)->primary = TRUE;
    }
    groups = g_list_sort(groups, compare_logical);

    g_variant_builder_init(&lm, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
    for (g = groups; g; g = g->next) {
        LogicalMonitor *c = g->data;
        GVariantBuilder mons;

        g_variant_builder_init(&mons, G_VARIANT_TYPE("a(ssa{sv})"));
        for (l = c->outputs; l; l = l->next) {
            DcOutput *o = l->data;
            const DcOutputMode *m =
                &g_array_index(o->modes, DcOutputMode, o->current_mode);
            GVariantBuilder mprops;

            g_variant_builder_init(&mprops, G_VARIANT_TYPE("a{sv}"));
            if (o->rgb_range > 0 && (o->rgb_ranges & (1u << o->rgb_range))) {
                g_variant_builder_add(&mprops, "{sv}", "rgb-range",
                                      g_variant_new_uint32(o->rgb_range));
            }
            if (o->color_format > 0 &&
                (o->color_formats & (1u << o->color_format))) {
                g_variant_builder_add(&mprops, "{sv}", "color-format",
                                      g_variant_new_uint32(o->color_format));
            }
            g_variant_builder_add(&mons, "(ss@a{sv})", o->name, m->id,
                                  g_variant_builder_end(&mprops));
        }
        g_variant_builder_add(&lm, "(iiduba(ssa{sv}))", c->x, c->y, c->scale,
                              (guint32) c->transform, c->primary, &mons);
        g_list_free(c->outputs);
    }
    g_list_free_full(groups, g_free);

    reply = call(p, "ApplyMonitorsConfig",
                 g_variant_new("(uua(iiduba(ssa{sv}))@a{sv})", p->serial,
                               (guint32) (persistent ? METHOD_PERSISTENT
                                                     : METHOD_TEMPORARY),
                               &lm,
                               g_variant_new_array(G_VARIANT_TYPE("{sv}"),
                                                   NULL, 0)),
                 NULL, error);
    if (!reply) {
        if (error && *error) {
            g_dbus_error_strip_remote_error(*error);
        }
        return FALSE;
    }
    g_variant_unref(reply);
    return TRUE;
}


/* ---- gamma -------------------------------------------------------------- */

/* The CRTC index (as GetCrtcGamma/SetCrtcGamma take it) currently driving
 * the output named 'name', and the serial to pass along.  Connector names
 * can repeat across GPUs; only one of them is driven. */
static gboolean find_crtc(MutterPriv *p, const gchar *name, guint32 *serial,
                          guint32 *crtc, GError **error)
{
    GVariant *reply, *outputs, *v;
    GVariantIter it;
    gboolean found = FALSE;

    reply = call(p, "GetResources", NULL,
                 G_VARIANT_TYPE("(ua(uxiiiiiuaua{sv})a(uxiausauaua{sv})"
                                "a(uxuudu)ii)"), error);
    if (!reply) {
        return FALSE;
    }
    g_variant_get(reply, "(u@a(uxiiiiiuaua{sv})@a(uxiausauaua{sv})"
                         "@a(uxuudu)ii)", serial, NULL, &outputs, NULL,
                  NULL, NULL);
    g_variant_iter_init(&it, outputs);
    while (!found && (v = g_variant_iter_next_value(&it))) {
        gint32 current_crtc;
        const gchar *oname;

        g_variant_get(v, "(uxi@au&s@au@au@a{sv})", NULL, NULL,
                      &current_crtc, NULL, &oname, NULL, NULL, NULL);
        if (current_crtc >= 0 && g_strcmp0(oname, name) == 0) {
            *crtc = (guint32) current_crtc;
            found = TRUE;
        }
        g_variant_unref(v);
    }
    g_variant_unref(outputs);
    g_variant_unref(reply);

    if (!found) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "%s is not turned on.", name);
    }
    return found;
}


static guint16 *ramp_from_variant(GVariant *v, gsize *n)
{
    gconstpointer data = g_variant_get_fixed_array(v, n, sizeof(guint16));
    return g_memdup2(data, *n * sizeof(guint16));
}


static gboolean mutter_get_gamma(DcBackend *b, const gchar *output,
                                 guint *size, guint16 **red, guint16 **green,
                                 guint16 **blue, GError **error)
{
    MutterPriv *p = b->priv;
    guint32 serial, crtc;
    GVariant *reply, *r, *g, *bl;
    gsize nr, ng, nb;

    if (!find_crtc(p, output, &serial, &crtc, error)) {
        return FALSE;
    }
    reply = call(p, "GetCrtcGamma", g_variant_new("(uu)", serial, crtc),
                 G_VARIANT_TYPE("(aqaqaq)"), error);
    if (!reply) {
        if (error && *error) {
            g_dbus_error_strip_remote_error(*error);
        }
        return FALSE;
    }
    g_variant_get(reply, "(@aq@aq@aq)", &r, &g, &bl);
    *red = ramp_from_variant(r, &nr);
    *green = ramp_from_variant(g, &ng);
    *blue = ramp_from_variant(bl, &nb);
    g_variant_unref(r);
    g_variant_unref(g);
    g_variant_unref(bl);
    g_variant_unref(reply);

    *size = MIN(nr, MIN(ng, nb));
    if (*size < 2) {
        g_free(*red);
        g_free(*green);
        g_free(*blue);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "The compositor reports no gamma ramp for %s.", output);
        return FALSE;
    }
    return TRUE;
}


static gboolean mutter_set_gamma(DcBackend *b, const gchar *output,
                                 guint size, const guint16 *red,
                                 const guint16 *green, const guint16 *blue,
                                 GError **error)
{
    MutterPriv *p = b->priv;
    guint32 serial, crtc;
    GVariant *reply;

    if (!find_crtc(p, output, &serial, &crtc, error)) {
        return FALSE;
    }
    reply = call(p, "SetCrtcGamma",
                 g_variant_new("(uu@aq@aq@aq)", serial, crtc,
                     g_variant_new_fixed_array(G_VARIANT_TYPE_UINT16, red,
                                               size, sizeof(guint16)),
                     g_variant_new_fixed_array(G_VARIANT_TYPE_UINT16, green,
                                               size, sizeof(guint16)),
                     g_variant_new_fixed_array(G_VARIANT_TYPE_UINT16, blue,
                                               size, sizeof(guint16))),
                 NULL, error);
    if (!reply) {
        if (error && *error) {
            g_dbus_error_strip_remote_error(*error);
        }
        return FALSE;
    }
    g_variant_unref(reply);
    return TRUE;
}


/* ---- construction ------------------------------------------------------- */

static void monitors_changed(GDBusConnection *bus, const gchar *sender,
                             const gchar *path, const gchar *iface,
                             const gchar *signal, GVariant *params,
                             gpointer data)
{
    dc_backend_notify_changed(data);
}


DcBackend *dc_backend_mutter_new(const gchar *bus_name,
                                 const gchar *object_path,
                                 const gchar *interface)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    MutterPriv *p;
    DcBackend *b;
    GList *outputs;
    GError *error = NULL;

    if (!bus) {
        return NULL;
    }

    p = g_new0(MutterPriv, 1);
    p->bus = bus;
    p->bus_name = g_strdup(bus_name);
    p->path = g_strdup(object_path);
    p->iface = g_strdup(interface);

    b = g_new0(DcBackend, 1);
    b->name = g_str_has_prefix(bus_name, "org.cinnamon")
              ? "Muffin DisplayConfig (D-Bus)"
              : "Mutter DisplayConfig (D-Bus)";
    b->query = mutter_query;
    b->apply = mutter_apply;
    b->get_gamma = mutter_get_gamma;
    b->set_gamma = mutter_set_gamma;
    b->priv = p;

    /* Is the interface there? */
    outputs = mutter_query(b, &error);
    if (error) {
        g_error_free(error);
        g_object_unref(bus);
        g_free(p->bus_name);
        g_free(p->path);
        g_free(p->iface);
        g_free(p);
        g_free(b);
        return NULL;
    }
    dc_output_list_free(outputs);

    p->signal_id = g_dbus_connection_signal_subscribe(
        bus, bus_name, interface, "MonitorsChanged", object_path, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, monitors_changed, b, NULL);
    return b;
}
