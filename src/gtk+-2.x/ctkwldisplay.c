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

#include <string.h>
#include <math.h>

#include <gtk/gtk.h>

#include "ctkwldisplay.h"
#include "ctkwlcolor.h"
#include "ctkbanner.h"
#include "ctkhelp.h"
#include "ctkutils.h"

#define WL_DISPLAY_UPDATE_INTERVAL_S 2

enum {
    MODE_COL_RESOLUTION,
    MODE_COL_RATES,
    MODE_NUM_COLS
};

static const char *__status_help =
"Whether the display is connected, and whether the compositor is currently "
"showing an image on it.";
static const char *__mode_help =
"The resolution and refresh rate the display is currently driven at, as "
"reported by the kernel.";
static const char *__vrr_help =
"Whether the monitor and GPU support Variable Refresh Rate (FreeSync / "
"VESA Adaptive-Sync / G-SYNC Compatible), and whether the compositor has "
"it enabled right now.  Most compositors only enable it for fullscreen "
"games, or when the option is turned on in the desktop's display "
"settings.";
static const char *__bpc_help =
"The maximum number of bits per color channel the GPU may send to the "
"monitor.  The compositor chooses this value.";
static const char *__colorspace_help =
"The colorimetry signalled to the monitor.  BT2020 variants are used for "
"HDR output.";
static const char *__rgb_help =
"The RGB range setting of the connector: Full (0-255), Limited (16-235), or "
"Automatic, where the driver sends limited range for TV modes (such as "
"1080p or 4K) and full range otherwise.";
static const char *__format_help =
"The pixel encoding setting of the connector: RGB or YCbCr, or Automatic, "
"where the driver chooses.";
static const char *__rgb_control_help =
"Selects the RGB range sent to the display.  A display that expects a "
"different range than it receives looks washed out (grey blacks) or too "
"dark (crushed shadows).  Automatic lets the driver choose: limited range "
"for TV modes, full range otherwise.  Choose Full for most monitors, and "
"for TVs set to full range (often called HDMI Black Level, RGB Range or "
"HDMI Dynamic Range).";
static const char *__format_control_help =
"Selects how pixels are encoded on the link: RGB, or YCbCr with full "
"(4:4:4) or reduced (4:2:2, 4:2:0) color resolution.  RGB 4:4:4 gives the "
"sharpest text; YCbCr 4:2:0 lets some displays run modes that otherwise "
"exceed the link's bandwidth.  Only the color spaces the display's EDID "
"allows for the current mode are listed, judged as the kernel driver does: "
"YCbCr 4:2:0 only for the progressive modes the display lists as 4:2:0 "
"capable, usually 4K TV modes, and never for interlaced modes.";
static const char *__controls_apply_help =
"Asks the compositor to apply the color range and color space, and to "
"remember them.  You will be asked to confirm the change; if you do not "
"confirm in time the previous setting is restored.";
static const char *__hdr_help =
"Whether the compositor is sending HDR metadata to the monitor, which "
"means HDR output is enabled.";
static const char *__modes_help =
"The modes the monitor advertises.  The current mode is marked with *, "
"the monitor's preferred mode with +.";
static const char *__enable_help =
"Turns this display on or off.";
static const char *__resolution_help =
"Selects the resolution of this display.  Click Apply to activate it.";
static const char *__rate_help =
"Selects the refresh rate for the chosen resolution.";
static const char *__scale_help =
"Scales everything shown on this display, for high resolution monitors.";
static const char *__rotation_help =
"Rotates the image shown on this display.";
static const char *__position_help =
"Position of the top left corner of this display in the desktop layout.  "
"When a display changes size, displays to its right and below move with "
"it, so the layout stays free of gaps.";
static const char *__primary_help =
"Makes this the primary display; desktop environments place panels and new "
"windows here.";
static const char *__vrr_policy_help =
"Whether the compositor may use Variable Refresh Rate (FreeSync / "
"Adaptive-Sync) on this display.  Automatic enables it for fullscreen "
"applications such as games.";
static const char *__apply_help =
"Asks the compositor to apply the settings above.  You will be asked to "
"confirm the change; if you do not confirm in time the previous "
"configuration is restored.";
static const char *__reset_help =
"Discards changes that have not been applied.";
static const char *__settings_help =
"Under Wayland the compositor owns the display configuration.  The "
"desktop's own display settings offer the same controls, and more (such "
"as mirroring or HDR) on some desktops.";


static gboolean backend_changed(gpointer data);


static void ctk_wl_display_class_init(CtkWlDisplayClass *klass)
{
}


GType ctk_wl_display_get_type(void)
{
    static GType type = 0;

    if (!type) {
        static const GTypeInfo info = {
            sizeof(CtkWlDisplayClass),
            NULL, NULL,
            (GClassInitFunc) ctk_wl_display_class_init,
            NULL, NULL,
            sizeof(CtkWlDisplay),
            0,
            NULL,
            NULL,
        };
        type = g_type_register_static(GTK_TYPE_VBOX, "CtkWlDisplay",
                                      &info, 0);
    }
    return type;
}


/* ---- helpers ------------------------------------------------------------ */

static GtkWidget *info_row(CtkWlDisplay *page, GtkWidget *table, gint *row,
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


static void set_label(GtkWidget *label, const gchar *text)
{
    if (label) {
        gtk_label_set_text(GTK_LABEL(label), text ? text : "Unknown");
    }
}


static void set_bool_label(GtkWidget *label, gint v, const gchar *yes,
                           const gchar *no)
{
    set_label(label, v < 0 ? "Unknown" : v ? yes : no);
}


/* ---- refresh ------------------------------------------------------------ */

static gchar *modes_signature(const DcConnector *c)
{
    GString *s = g_string_new(NULL);
    guint i;

    for (i = 0; i < c->modes->len; i++) {
        const DcMode *m = &g_array_index(c->modes, DcMode, i);
        g_string_append_printf(s, "%ux%u@%.2f%c,", m->width, m->height,
                               m->refresh, m->interlaced ? 'i' : 'p');
    }
    if (c->active) {
        g_string_append_printf(s, "|%ux%u@%.2f", c->current.width,
                               c->current.height, c->current.refresh);
    }
    return g_string_free(s, FALSE);
}


static gboolean same_mode(const DcMode *a, const DcMode *b)
{
    return a->width == b->width && a->height == b->height &&
           a->interlaced == b->interlaced &&
           fabs(a->refresh - b->refresh) < 0.005;
}


static gint compare_mode_order(gconstpointer a, gconstpointer b)
{
    const DcMode *x = a, *y = b;

    if (x->width * x->height != y->width * y->height) {
        return (x->width * x->height < y->width * y->height) ? 1 : -1;
    }
    if (x->width != y->width) {
        return x->width < y->width ? 1 : -1;
    }
    if (x->interlaced != y->interlaced) {
        return x->interlaced ? 1 : -1;
    }
    if (x->refresh != y->refresh) {
        return x->refresh < y->refresh ? 1 : -1;
    }
    return 0;
}


/*
 * One row per resolution, listing its refresh rates.  Modes that differ
 * only in timing details show up once, and the current mode is included
 * even when it is not one the monitor advertises (a custom mode).
 */
static void fill_modes(CtkWlDisplay *page, const DcConnector *c)
{
    GArray *modes = g_array_new(FALSE, FALSE, sizeof(DcMode));
    GtkTreeIter iter;
    gboolean have_current = FALSE;
    guint i = 0, k;

    for (k = 0; k < c->modes->len; k++) {
        DcMode m = g_array_index(c->modes, DcMode, k);
        gboolean dup = FALSE;
        guint n;

        if (c->active && same_mode(&m, &c->current)) {
            have_current = TRUE;
        }
        for (n = 0; n < modes->len; n++) {
            DcMode *o = &g_array_index(modes, DcMode, n);
            if (o->width == m.width && o->height == m.height &&
                o->interlaced == m.interlaced &&
                fabs(o->refresh - m.refresh) < 0.005) {
                o->preferred |= m.preferred;
                dup = TRUE;
                break;
            }
        }
        if (!dup) {
            g_array_append_val(modes, m);
        }
    }
    if (c->active && !have_current) {
        DcMode m = c->current;
        m.preferred = FALSE;
        g_array_append_val(modes, m);
    }
    g_array_sort(modes, compare_mode_order);

    gtk_list_store_clear(page->mode_store);
    while (i < modes->len) {
        const DcMode *first = &g_array_index(modes, DcMode, i);
        GString *rates = g_string_new(NULL);
        gchar *res;
        guint j;

        for (j = i; j < modes->len; j++) {
            const DcMode *m = &g_array_index(modes, DcMode, j);
            if (m->width != first->width || m->height != first->height ||
                m->interlaced != first->interlaced) {
                break;
            }
            if (rates->len) g_string_append(rates, ", ");
            if (m->refresh > 0) {
                g_string_append_printf(rates, "%.2f Hz", m->refresh);
            }
            if (c->active && same_mode(m, &c->current)) {
                g_string_append(rates, " *");
            }
            if (m->preferred) {
                g_string_append(rates, " +");
            }
        }
        res = dc_resolution_string(first->width, first->height,
                                   first->interlaced);
        gtk_list_store_append(page->mode_store, &iter);
        gtk_list_store_set(page->mode_store, &iter,
                           MODE_COL_RESOLUTION, res,
                           MODE_COL_RATES, g_strstrip(rates->str), -1);
        g_free(res);
        g_string_free(rates, TRUE);
        i = j;
    }
    g_array_free(modes, TRUE);
}


void ctk_wl_display_refresh(CtkWlDisplay *page)
{
    DcConnector *c = dc_kms_connector(page->card, page->connector);
    gchar *s;

    if (!c) {
        set_label(page->status_label, "Disconnected");
        return;
    }

    set_label(page->status_label,
              !c->connected ? "Disconnected" :
              c->active ? "Connected, active" : "Connected, off");

    if (c->active) {
        s = dc_mode_string(&c->current);
        set_label(page->mode_label, s);
        g_free(s);
    } else {
        set_label(page->mode_label, "None (display is off)");
    }

    if (c->vrr_capable < 0) {
        set_label(page->vrr_label, "Unknown");
    } else if (!c->vrr_capable) {
        set_label(page->vrr_label, "Not supported");
    } else {
        set_label(page->vrr_label,
                  c->vrr_enabled > 0 ? "Supported, active" :
                  c->vrr_enabled == 0 ? "Supported, inactive" : "Supported");
    }

    if (c->max_bpc > 0) {
        s = g_strdup_printf("%d", c->max_bpc);
        set_label(page->bpc_label, s);
        g_free(s);
    } else {
        set_label(page->bpc_label, NULL);
    }
    set_label(page->colorspace_label, c->colorspace);
    set_label(page->rgb_label, c->rgb_range);
    set_label(page->format_label, c->color_format);
    set_bool_label(page->hdr_label, c->hdr_metadata, "Enabled", "Disabled");

    s = modes_signature(c);
    if (g_strcmp0(s, page->modes_sig) != 0) {
        fill_modes(page, c);
        g_free(page->modes_sig);
        page->modes_sig = s;
    } else {
        g_free(s);
    }

    dc_connector_free(c);
}


static gboolean refresh_tick(gpointer data)
{
    ctk_wl_display_refresh(CTK_WL_DISPLAY(data));
    return TRUE;
}


void ctk_wl_display_start_timer(GtkWidget *widget)
{
    CtkWlDisplay *page = CTK_WL_DISPLAY(widget);
    guint *id = g_object_get_data(G_OBJECT(page), "refresh-timer");

    ctk_wl_display_refresh(page);
    if (!id) {
        id = g_new0(guint, 1);
        g_object_set_data_full(G_OBJECT(page), "refresh-timer", id, g_free);
    }
    if (!*id) {
        *id = g_timeout_add_seconds(WL_DISPLAY_UPDATE_INTERVAL_S,
                                    refresh_tick, page);
    }
}


void ctk_wl_display_stop_timer(GtkWidget *widget)
{
    guint *id = g_object_get_data(G_OBJECT(widget), "refresh-timer");

    if (id && *id) {
        g_source_remove(*id);
        *id = 0;
    }
}


static void settings_clicked(GtkButton *button, gpointer data)
{
    GError *error = NULL;

    if (!dc_open_desktop_display_settings(&error)) {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Unable to open the display settings: %s",
            error ? error->message : "unknown error");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        g_clear_error(&error);
    }
}


static void page_destroyed(GObject *obj, gpointer data)
{
    CtkWlDisplay *page = CTK_WL_DISPLAY(obj);

    ctk_wl_display_stop_timer(GTK_WIDGET(page));
    g_free(page->card);
    g_free(page->connector);
    g_free(page->key);
    g_free(page->title);
    g_free(page->gpu_label);
    g_free(page->modes_sig);
    page->card = page->connector = page->key = page->title = NULL;
    page->gpu_label = page->modes_sig = NULL;
    if (page->mode_store) {
        g_object_unref(page->mode_store);
        page->mode_store = NULL;
    }
    if (page->backend) {
        dc_backend_remove_watch(page->backend, backend_changed, page);
        page->backend = NULL;
    }
    dc_connector_free(page->kms_info);
    dc_output_free(page->output);
    page->kms_info = NULL;
    page->output = NULL;
    if (page->resolutions) g_array_free(page->resolutions, TRUE);
    if (page->rates) g_array_free(page->rates, TRUE);
    if (page->scales) g_array_free(page->scales, TRUE);
    if (page->vrr_values) g_array_free(page->vrr_values, TRUE);
    page->resolutions = page->rates = page->scales = page->vrr_values = NULL;
    if (page->verify_id) {
        g_source_remove(page->verify_id);
        page->verify_id = 0;
    }
    if (page->rgb_values) g_array_free(page->rgb_values, TRUE);
    if (page->format_values) g_array_free(page->format_values, TRUE);
    page->rgb_values = page->format_values = NULL;
    g_free(page->cc_output);
    page->cc_output = NULL;
}


/* ---- configuration through the compositor ------------------------------- */

static const gchar *rotation_names[] = { "Normal", "Left", "Inverted",
                                         "Right" };

static const gdouble preset_scales[] = { 1.0, 1.25, 1.5, 1.75, 2.0, 2.25,
                                         2.5, 2.75, 3.0 };

#define CONFIRM_TIMEOUT_SECONDS 15


static DcOutput *find_output(CtkWlDisplay *page, GList *outputs)
{
    if (!page->kms_info) {
        return NULL;
    }
    return dc_output_for_connector(outputs, page->kms_info,
                                   page->unique_name);
}


static gboolean usable_mode(const DcOutputMode *m)
{
    /* Mutter's variable refresh twins are chosen through the VRR control */
    return !m->variable;
}


static void combo_clear(GtkWidget *combo)
{
    gtk_list_store_clear(GTK_LIST_STORE(
        gtk_combo_box_get_model(GTK_COMBO_BOX(combo))));
}


static const DcOutputMode *output_mode(const DcOutput *o)
{
    if (!o || o->current_mode < 0 || o->current_mode >= (gint) o->modes->len) {
        return NULL;
    }
    return &g_array_index(o->modes, DcOutputMode, o->current_mode);
}


/* The mode of 'o' matching the selection, preferring the requested VRR
 * variant (Mutter), or -1. */
static gint find_mode(const DcOutput *o, guint w, guint h, gboolean il,
                      gdouble refresh, gboolean variable)
{
    gint i, fallback = -1;

    for (i = 0; i < (gint) o->modes->len; i++) {
        const DcOutputMode *m = &g_array_index(o->modes, DcOutputMode, i);
        if (m->width != w || m->height != h || m->interlaced != il ||
            fabs(m->refresh - refresh) >= 0.005) {
            continue;
        }
        if (m->variable == variable) {
            return i;
        }
        if (!m->variable) {
            fallback = i;
        }
    }
    return fallback;
}


static void fill_scales(CtkWlDisplay *page, const DcOutputMode *mode,
                        gdouble select)
{
    gint i, active = -1;
    gdouble best = G_MAXDOUBLE;

    page->updating = TRUE;
    combo_clear(page->scale_combo);
    g_array_set_size(page->scales, 0);

    if (mode && mode->scales && mode->scales->len) {
        g_array_append_vals(page->scales, mode->scales->data,
                            mode->scales->len);
    } else {
        gboolean have = FALSE;
        for (i = 0; i < (gint) G_N_ELEMENTS(preset_scales); i++) {
            g_array_append_val(page->scales, preset_scales[i]);
            have |= fabs(preset_scales[i] - select) < 0.001;
        }
        if (!have && select > 0) {
            g_array_append_val(page->scales, select);
        }
    }

    for (i = 0; i < (gint) page->scales->len; i++) {
        gdouble s = g_array_index(page->scales, gdouble, i);
        gchar *text = g_strdup_printf("%.0f%%", s * 100.0);
        ctk_combo_box_text_append_text(page->scale_combo, text);
        g_free(text);
        if (fabs(s - select) < best) {
            best = fabs(s - select);
            active = i;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(page->scale_combo), active);
    page->updating = FALSE;
}


static gdouble selected_scale(CtkWlDisplay *page)
{
    gint i = gtk_combo_box_get_active(GTK_COMBO_BOX(page->scale_combo));
    if (i < 0 || i >= (gint) page->scales->len) {
        return page->output ? page->output->scale : 1.0;
    }
    return g_array_index(page->scales, gdouble, i);
}


static const DcMode *selected_resolution(CtkWlDisplay *page)
{
    gint i = gtk_combo_box_get_active(GTK_COMBO_BOX(page->resolution_combo));
    if (i < 0 || i >= (gint) page->resolutions->len) {
        return NULL;
    }
    return &g_array_index(page->resolutions, DcMode, i);
}


/* The output mode for the current resolution and refresh selection */
static const DcOutputMode *selected_mode(CtkWlDisplay *page, gint *index)
{
    const DcMode *r = selected_resolution(page);
    gint i = gtk_combo_box_get_active(GTK_COMBO_BOX(page->rate_combo));
    gint m;

    if (!r || !page->output || i < 0 || i >= (gint) page->rates->len) {
        return NULL;
    }
    m = find_mode(page->output, r->width, r->height, r->interlaced,
                  g_array_index(page->rates, gdouble, i), FALSE);
    if (index) *index = m;
    return m >= 0 ? &g_array_index(page->output->modes, DcOutputMode, m)
                  : NULL;
}


static void fill_rates(CtkWlDisplay *page, gdouble select)
{
    const DcMode *r = selected_resolution(page);
    const DcOutput *o = page->output;
    gint i, active = -1, preferred = -1;

    page->updating = TRUE;
    combo_clear(page->rate_combo);
    g_array_set_size(page->rates, 0);

    for (i = 0; r && o && i < (gint) o->modes->len; i++) {
        const DcOutputMode *m = &g_array_index(o->modes, DcOutputMode, i);
        gboolean dup = FALSE;
        guint j;
        gchar *text;

        if (!usable_mode(m) || m->width != r->width ||
            m->height != r->height || m->interlaced != r->interlaced) {
            continue;
        }
        for (j = 0; j < page->rates->len; j++) {
            if (fabs(g_array_index(page->rates, gdouble, j) - m->refresh)
                < 0.005) {
                dup = TRUE;
            }
        }
        if (dup) continue;

        g_array_append_val(page->rates, m->refresh);
        text = m->refresh > 0
               ? g_strdup_printf("%.2f Hz%s", m->refresh,
                                 m->preferred ? " (preferred)" : "")
               : g_strdup("Automatic");
        ctk_combo_box_text_append_text(page->rate_combo, text);
        g_free(text);
        if (fabs(m->refresh - select) < 0.005) {
            active = page->rates->len - 1;
        }
        if (m->preferred) {
            preferred = page->rates->len - 1;
        }
    }
    if (active < 0) {
        active = preferred >= 0 ? preferred : (page->rates->len ? 0 : -1);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(page->rate_combo), active);
    page->updating = FALSE;
}


static gint compare_resolutions(gconstpointer a, gconstpointer b)
{
    const DcMode *x = a, *y = b;
    if (x->width * x->height != y->width * y->height) {
        return (x->width * x->height < y->width * y->height) ? 1 : -1;
    }
    if (x->width != y->width) {
        return x->width < y->width ? 1 : -1;
    }
    return x->interlaced - y->interlaced;
}


static void fill_resolutions(CtkWlDisplay *page, const DcOutputMode *select)
{
    const DcOutput *o = page->output;
    gint i, active = -1;

    page->updating = TRUE;
    combo_clear(page->resolution_combo);
    g_array_set_size(page->resolutions, 0);

    for (i = 0; o && i < (gint) o->modes->len; i++) {
        const DcOutputMode *m = &g_array_index(o->modes, DcOutputMode, i);
        DcMode r = { 0 };
        guint j;
        gboolean dup = FALSE;

        if (!usable_mode(m)) continue;
        for (j = 0; j < page->resolutions->len; j++) {
            const DcMode *e = &g_array_index(page->resolutions, DcMode, j);
            if (e->width == m->width && e->height == m->height &&
                e->interlaced == m->interlaced) {
                dup = TRUE;
                if (m->preferred) {
                    ((DcMode *) e)->preferred = TRUE;
                }
            }
        }
        if (dup) continue;
        r.width = m->width;
        r.height = m->height;
        r.interlaced = m->interlaced;
        r.preferred = m->preferred;
        g_array_append_val(page->resolutions, r);
    }
    g_array_sort(page->resolutions, compare_resolutions);

    for (i = 0; i < (gint) page->resolutions->len; i++) {
        const DcMode *r = &g_array_index(page->resolutions, DcMode, i);
        gchar *res = dc_resolution_string(r->width, r->height,
                                          r->interlaced);
        gchar *text = g_strdup_printf("%s%s", res,
                                      r->preferred ? ", preferred" : "");
        ctk_combo_box_text_append_text(page->resolution_combo, text);
        g_free(text);
        g_free(res);
        if (select && r->width == select->width &&
            r->height == select->height &&
            r->interlaced == select->interlaced) {
            active = i;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(page->resolution_combo), active);
    page->updating = FALSE;
}


static void fill_vrr(CtkWlDisplay *page, const DcOutput *o)
{
    static const struct { DcVrr v; const gchar *label; } items[] = {
        { DC_VRR_NEVER,     "Off" },
        { DC_VRR_AUTOMATIC, "Automatic (fullscreen applications)" },
        { DC_VRR_ALWAYS,    "Always on" },
    };
    gint i, active = -1;

    page->updating = TRUE;
    combo_clear(page->vrr_combo);
    g_array_set_size(page->vrr_values, 0);
    for (i = 0; o && i < (gint) G_N_ELEMENTS(items); i++) {
        gint v = items[i].v;
        if (!(o->vrr_policies & (1u << v))) continue;
        g_array_append_val(page->vrr_values, v);
        ctk_combo_box_text_append_text(page->vrr_combo, items[i].label);
        if (o->vrr == items[i].v) {
            active = page->vrr_values->len - 1;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(page->vrr_combo), active);
    gtk_widget_set_sensitive(page->vrr_combo, page->vrr_values->len > 1);
    if (page->vrr_values->len) {
        gtk_widget_show(page->vrr_combo);
        gtk_widget_hide(page->vrr_unsupported);
    } else {
        gtk_widget_hide(page->vrr_combo);
        gtk_widget_show(page->vrr_unsupported);
    }
    page->updating = FALSE;
}


static void update_sensitivity(CtkWlDisplay *page)
{
    gboolean have = page->output != NULL;
    gboolean on = have &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->enable_check));

    gtk_widget_set_sensitive(page->enable_check,
                             have && (page->backend->caps & DC_CAP_DISABLE));
    gtk_widget_set_sensitive(page->resolution_combo, on);
    gtk_widget_set_sensitive(page->rate_combo, on);
    gtk_widget_set_sensitive(page->scale_combo, on);
    gtk_widget_set_sensitive(page->rotation_combo, on);
    gtk_widget_set_sensitive(page->pos_x_spin, on);
    gtk_widget_set_sensitive(page->pos_y_spin, on);
    gtk_widget_set_sensitive(page->vrr_combo,
                             on && page->vrr_values->len > 1);
    /* There is always a primary display; pick another one to change it */
    gtk_widget_set_sensitive(page->primary_check,
                             on && !(page->output && page->output->primary));
    gtk_widget_set_sensitive(page->apply_button, have);
    gtk_widget_set_sensitive(page->reset_button, have);
}


typedef struct {
    gint value;
    const gchar *label;
} Choice;

static const Choice rgb_choices[] = {
    { DC_RGB_RANGE_AUTO,    "Automatic" },
    { DC_RGB_RANGE_FULL,    "Full (0-255)" },
    { DC_RGB_RANGE_LIMITED, "Limited (16-235)" },
};

static const Choice format_choices[] = {
    { DC_COLOR_FORMAT_AUTO,     "Automatic" },
    { DC_COLOR_FORMAT_RGB,      "RGB" },
    { DC_COLOR_FORMAT_YCBCR444, "YCbCr 4:4:4" },
    { DC_COLOR_FORMAT_YCBCR422, "YCbCr 4:2:2" },
    { DC_COLOR_FORMAT_YCBCR420, "YCbCr 4:2:0" },
};


static void fill_choice(CtkWlDisplay *page, GtkWidget *combo, GArray *values,
                        const Choice *choices, gint n, guint mask,
                        gint current)
{
    gint i, active = -1;

    page->updating = TRUE;
    combo_clear(combo);
    g_array_set_size(values, 0);
    for (i = 0; i < n; i++) {
        if (!(mask & (1u << choices[i].value))) continue;
        g_array_append_val(values, choices[i].value);
        ctk_combo_box_text_append_text(combo, choices[i].label);
        if (choices[i].value == current) {
            active = values->len - 1;
        }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    gtk_widget_set_sensitive(combo, values->len > 1);
    page->updating = FALSE;
}


static gint selected_choice(GtkWidget *combo, GArray *values)
{
    gint i = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
    return (i >= 0 && i < (gint) values->len)
           ? g_array_index(values, gint, i) : 0;
}


/* The color formats worth offering for the output's current mode: the ones
 * the compositor can set and the monitor's EDID allows.  Drivers refuse the
 * others (YCbCr 4:2:0 for a mode without 4:2:0 support, for example). */
static guint offered_color_formats(CtkWlDisplay *page, const DcOutput *o)
{
    const DcOutputMode *om = output_mode(o);
    const DcConnector *c = page->kms_info;
    DcMode mode = { 0 };
    guint allowed;

    if (om) {
        mode.width = om->width;
        mode.height = om->height;
        mode.refresh = om->refresh;
        mode.interlaced = om->interlaced;
    }
    allowed = dc_edid_color_formats(c ? c->edid : NULL,
                                    c ? c->edid_len : 0,
                                    om ? &mode : NULL);

    /* keep showing the current one */
    if (o->color_format > 0) {
        allowed |= 1u << o->color_format;
    }
    return o->color_formats & allowed;
}


static void fill_color_controls(CtkWlDisplay *page, const DcOutput *o)
{
    gboolean any;

    if (!page->controls_table) {
        return;
    }
    fill_choice(page, page->rgb_combo, page->rgb_values, rgb_choices,
                G_N_ELEMENTS(rgb_choices), o ? o->rgb_ranges : 0,
                o ? o->rgb_range : 0);
    fill_choice(page, page->format_combo, page->format_values,
                format_choices, G_N_ELEMENTS(format_choices),
                o ? offered_color_formats(page, o) : 0,
                o ? o->color_format : 0);

    any = o && (o->rgb_ranges || o->color_formats);
    if (any) {
        gtk_label_set_text(GTK_LABEL(page->controls_status),
                           g_object_get_data(G_OBJECT(page), "status-text"));
    } else {
        gchar *s = g_strdup_printf("%s does not offer these settings for "
                                   "this display.  On Cinnamon they need "
                                   "Muffin with color range support.",
                                   page->backend->name);
        gtk_label_set_text(GTK_LABEL(page->controls_status), s);
        g_free(s);
    }
    gtk_widget_set_sensitive(page->controls_apply, any);
    gtk_widget_set_sensitive(page->controls_reset, any);
}


/* (Re)build the Color Correction tab for the output the compositor shows
 * this display on */
static void update_color_correction(CtkWlDisplay *page)
{
    const gchar *name = page->output ? page->output->name : NULL;
    GList *children, *l;
    GtkWidget *w;

    if (!page->cc_box || (page->cc_output && !name) ||
        (name && g_strcmp0(name, page->cc_output) == 0)) {
        return;
    }
    children = gtk_container_get_children(GTK_CONTAINER(page->cc_box));
    for (l = children; l; l = l->next) {
        gtk_widget_destroy(l->data);
    }
    g_list_free(children);

    g_free(page->cc_output);
    page->cc_output = g_strdup(name);
    if (name) {
        w = ctk_wl_color_correction_new(page->backend, name,
                                        page->ctk_config);
    } else {
        w = gtk_label_new("The compositor does not list this display.");
        gtk_misc_set_alignment(GTK_MISC(w), 0.0f, 0.0f);
    }
    gtk_box_pack_start(GTK_BOX(page->cc_box), w, FALSE, FALSE, 0);
    gtk_widget_show_all(page->cc_box);
}


/* Read the compositor's current state into the controls */
static void load_config(CtkWlDisplay *page)
{
    GError *error = NULL;
    GList *outputs;
    DcOutput *o;
    const DcOutputMode *mode;

    if (!page->backend) {
        return;
    }
    outputs = dc_backend_outputs(page->backend, &error);
    o = find_output(page, outputs);

    dc_output_free(page->output);
    page->output = o ? dc_output_copy(o) : NULL;
    dc_output_list_free(outputs);

    if (!page->output) {
        gchar *s = g_strdup_printf("The compositor does not list this "
                                   "display%s%s",
                                   error ? ": " : ".",
                                   error ? error->message : "");
        gtk_label_set_text(GTK_LABEL(page->config_status), s);
        g_free(s);
        g_clear_error(&error);
        gtk_widget_hide(page->config_table);
        update_sensitivity(page);
        fill_color_controls(page, NULL);
        update_color_correction(page);
        return;
    }
    gtk_widget_show(page->config_table);
    gtk_label_set_text(GTK_LABEL(page->config_status),
                       g_object_get_data(G_OBJECT(page), "status-text"));
    o = page->output;
    mode = output_mode(o);

    page->updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->enable_check),
                                 o->enabled);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->primary_check),
                                 o->primary);
    gtk_combo_box_set_active(GTK_COMBO_BOX(page->rotation_combo),
                             o->transform & 3);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->pos_x_spin), o->x);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->pos_y_spin), o->y);
    page->updating = FALSE;

    fill_resolutions(page, mode);
    fill_rates(page, mode ? mode->refresh : 0);
    fill_scales(page, mode, o->scale);
    fill_vrr(page, o);
    update_sensitivity(page);
    fill_color_controls(page, o);
    update_color_correction(page);
}


static void mark_dirty(CtkWlDisplay *page, gboolean dirty)
{
    g_object_set_data(G_OBJECT(page), "dirty", GINT_TO_POINTER(dirty));
}


static gboolean backend_changed(gpointer data)
{
    CtkWlDisplay *page = data;

    /* Don't throw away selections the user has not applied yet */
    if (!g_object_get_data(G_OBJECT(page), "dirty")) {
        load_config(page);
    }
    return FALSE;
}


static void resolution_changed(GtkComboBox *combo, gpointer data)
{
    CtkWlDisplay *page = data;
    gdouble refresh = 0;
    gint i;

    if (page->updating) return;
    i = gtk_combo_box_get_active(GTK_COMBO_BOX(page->rate_combo));
    if (i >= 0 && i < (gint) page->rates->len) {
        refresh = g_array_index(page->rates, gdouble, i);
    }
    fill_rates(page, refresh);
    fill_scales(page, selected_mode(page, NULL), selected_scale(page));
    mark_dirty(page, TRUE);
}


static void rate_changed(GtkComboBox *combo, gpointer data)
{
    CtkWlDisplay *page = data;

    if (page->updating) return;
    /* Mutter allows different scales per mode */
    fill_scales(page, selected_mode(page, NULL), selected_scale(page));
    mark_dirty(page, TRUE);
}


static void control_changed(GtkWidget *w, gpointer data)
{
    CtkWlDisplay *page = data;

    if (page->updating) return;
    mark_dirty(page, TRUE);
    update_sensitivity(page);
}


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


static gboolean confirm_configuration(CtkWlDisplay *page)
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


static void error_dialog(CtkWlDisplay *page, const gchar *what,
                         const GError *error)
{
    GtkWidget *parent = ctk_get_parent_window(GTK_WIDGET(page));
    GtkWidget *dlg = gtk_message_dialog_new(
        parent ? GTK_WINDOW(parent) : NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s: %s", what,
        error ? error->message : "unknown error");
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}


/* Build the requested configuration from the controls.  Returns the new
 * full output list, or NULL with 'error'. */
static GList *build_config(CtkWlDisplay *page, GList *current,
                           GError **error)
{
    GList *outputs = dc_output_list_copy(current), *l;
    DcOutput *o = find_output(page, outputs);
    DcOutput *before;
    gboolean enable, pos_changed;
    gint mode_index = -1, x, y, vi;
    gdouble scale;

    if (!o) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "The compositor no longer lists this display.");
        dc_output_list_free(outputs);
        return NULL;
    }
    before = dc_output_copy(o);

    enable = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(page->enable_check));
    x = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page->pos_x_spin));
    y = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page->pos_y_spin));
    pos_changed = (x != o->x || y != o->y);

    if (enable) {
        const DcMode *r = selected_resolution(page);
        gint ri = gtk_combo_box_get_active(GTK_COMBO_BOX(page->rate_combo));

        vi = gtk_combo_box_get_active(GTK_COMBO_BOX(page->vrr_combo));
        if (vi >= 0 && vi < (gint) page->vrr_values->len) {
            o->vrr = g_array_index(page->vrr_values, gint, vi);
        }
        if (r && ri >= 0 && ri < (gint) page->rates->len) {
            mode_index = find_mode(o, r->width, r->height, r->interlaced,
                                   g_array_index(page->rates, gdouble, ri),
                                   o->vrr == DC_VRR_ALWAYS);
        }
        if (mode_index < 0) {
            g_set_error_literal(error, G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "Select a resolution and refresh rate.");
            dc_output_list_free(outputs);
            dc_output_free(before);
            return NULL;
        }
        o->enabled = TRUE;
        o->current_mode = mode_index;
        o->transform = (o->transform & 4) |
            MAX(0, gtk_combo_box_get_active(
                       GTK_COMBO_BOX(page->rotation_combo)));
        o->x = x;
        o->y = y;

        scale = selected_scale(page);
        o->scale = scale;
        if (page->backend->caps & DC_CAP_GLOBAL_SCALE) {
            for (l = outputs; l; l = l->next) {
                ((DcOutput *) l->data)->scale = scale;
            }
        }
        if (gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(page->primary_check))) {
            for (l = outputs; l; l = l->next) {
                ((DcOutput *) l->data)->primary = (l->data == o);
            }
        }
    } else {
        gint others = 0;
        for (l = outputs; l; l = l->next) {
            DcOutput *e = l->data;
            if (e != o && e->enabled) others++;
        }
        if (!others) {
            g_set_error_literal(error, G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "This is the only enabled display; it "
                                "cannot be turned off.");
            dc_output_list_free(outputs);
            dc_output_free(before);
            return NULL;
        }
        o->enabled = FALSE;
        if (o->primary) {
            o->primary = FALSE;
            for (l = outputs; l; l = l->next) {
                DcOutput *e = l->data;
                if (e->enabled) {
                    e->primary = TRUE;
                    break;
                }
            }
        }
    }

    /* Keep the layout free of gaps when the display changes size, is turned
     * on or off; if the user moved it, only normalize the origin. */
    dc_layout_fixup(page->backend, outputs, o, before, pos_changed);
    dc_output_free(before);
    return outputs;
}


/* Apply 'config', confirm it (or let the desktop do so) and save it.
 * Frees both lists. */
static void run_apply(CtkWlDisplay *page, GList *before, GList *config,
                      GtkWidget *button)
{
    DcBackend *b = page->backend;
    GError *error = NULL;

    gtk_widget_set_sensitive(button, FALSE);
    if (b->caps & DC_CAP_COMPOSITOR_CONFIRMS) {
        /* The desktop shows its own keep/revert prompt */
        if (!dc_backend_apply(b, config, TRUE, &error)) {
            error_dialog(page, "Unable to apply the display configuration",
                         error);
            g_clear_error(&error);
        }
    } else if (!dc_backend_apply(b, config, FALSE, &error)) {
        error_dialog(page, "Unable to apply the display configuration",
                     error);
        g_clear_error(&error);
    } else if (!confirm_configuration(page)) {
        if (!dc_backend_apply(b, before, FALSE, &error)) {
            error_dialog(page, "Unable to restore the previous display "
                         "configuration", error);
            g_clear_error(&error);
        }
    } else if (b->caps & DC_CAP_PERSIST) {
        /* Ask the compositor to remember the kept configuration */
        GList *now = b->query(b, NULL);
        if (now && !dc_backend_apply(b, now, TRUE, &error)) {
            error_dialog(page, "The configuration was applied but could "
                         "not be saved", error);
            g_clear_error(&error);
        }
        dc_output_list_free(now);
    }

    dc_output_list_free(config);
    dc_output_list_free(before);
    mark_dirty(page, FALSE);
    load_config(page);
}


static void apply_clicked(GtkButton *button, gpointer data)
{
    CtkWlDisplay *page = data;
    DcBackend *b = page->backend;
    GError *error = NULL;
    GList *before, *config;

    before = b->query(b, &error);
    if (!before) {
        error_dialog(page, "Unable to read the display configuration", error);
        g_clear_error(&error);
        return;
    }
    config = build_config(page, before, &error);
    if (!config) {
        error_dialog(page, "Unable to apply the display configuration",
                     error);
        g_clear_error(&error);
        dc_output_list_free(before);
        return;
    }
    run_apply(page, before, config, page->apply_button);
}


/* The value of a KMS "Broadcast RGB" / "color format" enum name */
static gint rgb_range_from_kms(const gchar *name)
{
    if (!name) return 0;
    if (g_ascii_strcasecmp(name, "Automatic") == 0) return DC_RGB_RANGE_AUTO;
    if (g_ascii_strcasecmp(name, "Full") == 0) return DC_RGB_RANGE_FULL;
    if (g_ascii_strncasecmp(name, "Limited", 7) == 0) {
        return DC_RGB_RANGE_LIMITED;
    }
    return 0;
}


static gint color_format_from_kms(const gchar *name)
{
    if (!name) return 0;
    if (g_ascii_strcasecmp(name, "AUTO") == 0) return DC_COLOR_FORMAT_AUTO;
    if (g_ascii_strcasecmp(name, "RGB") == 0) return DC_COLOR_FORMAT_RGB;
    if (strstr(name, "4:4:4")) return DC_COLOR_FORMAT_YCBCR444;
    if (strstr(name, "4:2:2")) return DC_COLOR_FORMAT_YCBCR422;
    if (strstr(name, "4:2:0")) return DC_COLOR_FORMAT_YCBCR420;
    return 0;
}


static const gchar *choice_label(const Choice *choices, gint n, gint value)
{
    gint i;

    for (i = 0; i < n; i++) {
        if (choices[i].value == value) return choices[i].label;
    }
    return "Unknown";
}


/* After applying, tell the user if the kernel kept other values: drivers
 * refuse settings the mode cannot be sent with (such as YCbCr 4:2:0 for a
 * mode without 4:2:0 support), and the compositor then keeps the old ones
 * while still reporting the requested ones. */
static gboolean verify_color_settings(gpointer data)
{
    CtkWlDisplay *page = data;
    DcConnector *c = dc_kms_connector(page->card, page->connector);
    GString *msg = g_string_new(NULL);
    gint rgb, format;

    page->verify_id = 0;
    if (!c) {
        g_string_free(msg, TRUE);
        return FALSE;
    }

    rgb = rgb_range_from_kms(c->rgb_range);
    format = color_format_from_kms(c->color_format);
    if (page->requested_rgb && rgb && rgb != page->requested_rgb) {
        g_string_append_printf(msg, "Color Range %s (it uses %s)",
                               choice_label(rgb_choices,
                                            G_N_ELEMENTS(rgb_choices),
                                            page->requested_rgb),
                               choice_label(rgb_choices,
                                            G_N_ELEMENTS(rgb_choices), rgb));
    }
    if (page->requested_format && format &&
        format != page->requested_format) {
        g_string_append_printf(msg, "%sColor Space %s (it uses %s)",
                               msg->len ? " or " : "",
                               choice_label(format_choices,
                                            G_N_ELEMENTS(format_choices),
                                            page->requested_format),
                               choice_label(format_choices,
                                            G_N_ELEMENTS(format_choices),
                                            format));
    }

    if (msg->len) {
        GtkWidget *parent = ctk_get_parent_window(GTK_WIDGET(page));
        gchar *mode = c->active ? dc_mode_string(&c->current)
                                : g_strdup("its current mode");
        GtkWidget *dlg = gtk_message_dialog_new(
            parent ? GTK_WINDOW(parent) : NULL,
            GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING,
            GTK_BUTTONS_CLOSE, "%s did not accept these color settings",
            page->title);

        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dlg),
            "The display or its driver refused %s at %s, so the previous "
            "settings stay in use.  If the desktop asks whether to keep the "
            "change, choose Revert.", msg->str, mode);
        g_signal_connect(G_OBJECT(dlg), "response",
                         G_CALLBACK(gtk_widget_destroy), NULL);
        gtk_widget_show(dlg);
        g_free(mode);
    }

    g_string_free(msg, TRUE);
    dc_connector_free(c);
    return FALSE;
}


/* The Controls tab applies only its own settings, on top of the current
 * configuration, so changes pending on the Information tab stay pending */
static void controls_apply_clicked(GtkButton *button, gpointer data)
{
    CtkWlDisplay *page = data;
    DcBackend *b = page->backend;
    GError *error = NULL;
    GList *before, *config;
    DcOutput *o;
    gint v;

    before = b->query(b, &error);
    if (!before) {
        error_dialog(page, "Unable to read the display configuration", error);
        g_clear_error(&error);
        return;
    }
    config = dc_output_list_copy(before);
    o = find_output(page, config);
    if (!o) {
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "The compositor no longer lists this display.");
        error_dialog(page, "Unable to apply the color settings", error);
        g_clear_error(&error);
        dc_output_list_free(config);
        dc_output_list_free(before);
        return;
    }
    page->requested_rgb = page->requested_format = 0;
    if ((v = selected_choice(page->rgb_combo, page->rgb_values))) {
        if (v != o->rgb_range) page->requested_rgb = v;
        o->rgb_range = v;
    }
    if ((v = selected_choice(page->format_combo, page->format_values))) {
        if (v != o->color_format) page->requested_format = v;
        o->color_format = v;
    }
    run_apply(page, before, config, page->controls_apply);

    /* The mode set happens with the compositor's next frame */
    if (page->verify_id) {
        g_source_remove(page->verify_id);
    }
    if (page->requested_rgb || page->requested_format) {
        page->verify_id = g_timeout_add(1500, verify_color_settings, page);
    }
}


static void controls_changed(GtkWidget *w, gpointer data)
{
    CtkWlDisplay *page = data;

    if (page->updating) return;
    mark_dirty(page, TRUE);
}


static void reset_clicked(GtkButton *button, gpointer data)
{
    CtkWlDisplay *page = data;
    mark_dirty(page, FALSE);
    load_config(page);
}


static gboolean connector_name_unique(const gchar *card, const gchar *name)
{
    GDir *dir = g_dir_open("/sys/class/drm", 0, NULL);
    const gchar *e;
    gchar *suffix = g_strconcat("-", name, NULL);
    gint hits = 0;

    while (dir && (e = g_dir_read_name(dir))) {
        const gchar *dash = strchr(e, '-');
        if (g_str_has_prefix(e, "card") && dash && strcmp(dash, suffix) == 0) {
            hits++;
        }
    }
    if (dir) g_dir_close(dir);
    g_free(suffix);
    return hits <= 1;
}


static GtkWidget *config_row(CtkWlDisplay *page, GtkWidget *table, gint *row,
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
    return hbox;
}


static void build_config_controls(CtkWlDisplay *page, GtkWidget *vbox)
{
    const DcCompositor *comp = dc_compositor();
    const gchar *desktop = comp && comp->desktop ? comp->desktop
                                                 : "the compositor";
    GtkWidget *table, *hbox;
    gint row = 0, i;
    gchar *s;

    page->resolutions = g_array_new(FALSE, FALSE, sizeof(DcMode));
    page->rates = g_array_new(FALSE, FALSE, sizeof(gdouble));
    page->scales = g_array_new(FALSE, FALSE, sizeof(gdouble));
    page->vrr_values = g_array_new(FALSE, FALSE, sizeof(gint));

    table = ctk_section_table_new(vbox, "Display Configuration");
    page->config_table = table;

    page->enable_check = gtk_check_button_new_with_label("Enabled");
    config_row(page, table, &row, "Display:", page->enable_check,
               __enable_help);
    page->resolution_combo = ctk_combo_box_text_new();
    config_row(page, table, &row, "Resolution:", page->resolution_combo,
               __resolution_help);
    page->rate_combo = ctk_combo_box_text_new();
    config_row(page, table, &row, "Refresh Rate:", page->rate_combo,
               __rate_help);
    page->scale_combo = ctk_combo_box_text_new();
    config_row(page, table, &row, "Scale:", page->scale_combo,
               __scale_help);
    page->rotation_combo = ctk_combo_box_text_new();
    for (i = 0; i < 4; i++) {
        ctk_combo_box_text_append_text(page->rotation_combo,
                                       rotation_names[i]);
    }
    config_row(page, table, &row, "Rotation:", page->rotation_combo,
               __rotation_help);

    page->pos_x_spin = gtk_spin_button_new_with_range(-32768, 32767, 1);
    page->pos_y_spin = gtk_spin_button_new_with_range(-32768, 32767, 1);
    hbox = config_row(page, table, &row, "Position:", page->pos_x_spin,
                      __position_help);
    gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(","), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), page->pos_y_spin, FALSE, FALSE, 0);
    ctk_config_set_tooltip(page->ctk_config, page->pos_y_spin,
                           __position_help);

    page->primary_check = gtk_check_button_new_with_label("Primary display");
    if (page->backend->caps & DC_CAP_PRIMARY) {
        config_row(page, table, &row, "Primary:", page->primary_check,
                   __primary_help);
    }
    page->vrr_combo = ctk_combo_box_text_new();
    hbox = config_row(page, table, &row, "Variable Refresh Rate:",
                      page->vrr_combo, __vrr_policy_help);
    page->vrr_unsupported = gtk_label_new("Not supported");
    gtk_box_pack_start(GTK_BOX(hbox), page->vrr_unsupported, FALSE, FALSE, 0);

    g_signal_connect(G_OBJECT(page->enable_check), "toggled",
                     G_CALLBACK(control_changed), page);
    g_signal_connect(G_OBJECT(page->resolution_combo), "changed",
                     G_CALLBACK(resolution_changed), page);
    g_signal_connect(G_OBJECT(page->rate_combo), "changed",
                     G_CALLBACK(rate_changed), page);
    g_signal_connect(G_OBJECT(page->scale_combo), "changed",
                     G_CALLBACK(control_changed), page);
    g_signal_connect(G_OBJECT(page->rotation_combo), "changed",
                     G_CALLBACK(control_changed), page);
    g_signal_connect(G_OBJECT(page->pos_x_spin), "value-changed",
                     G_CALLBACK(control_changed), page);
    g_signal_connect(G_OBJECT(page->pos_y_spin), "value-changed",
                     G_CALLBACK(control_changed), page);
    g_signal_connect(G_OBJECT(page->primary_check), "toggled",
                     G_CALLBACK(control_changed), page);
    g_signal_connect(G_OBJECT(page->vrr_combo), "changed",
                     G_CALLBACK(control_changed), page);

    /* buttons and status */
    hbox = gtk_hbox_new(FALSE, 5);
    if (page->backend->caps & DC_CAP_COMPOSITOR_CONFIRMS) {
        s = g_strdup_printf("Applied through %s; %s asks you to keep the "
                            "change and then saves it.", page->backend->name,
                            desktop);
    } else if (page->backend->caps & DC_CAP_SAVED_BY_COMPOSITOR) {
        s = g_strdup_printf("Applied through %s; %s saves the settings it "
                            "keeps.", page->backend->name, desktop);
    } else if (page->backend->caps & DC_CAP_PERSIST) {
        s = g_strdup_printf("Applied through %s; kept settings are saved "
                            "by %s.", page->backend->name, desktop);
    } else {
        s = g_strdup_printf("Applied through %s.  Changes last until %s "
                            "restarts; add them to its configuration file "
                            "to keep them.", page->backend->name, desktop);
    }
    page->config_status = gtk_label_new(s);
    g_object_set_data_full(G_OBJECT(page), "status-text", s, g_free);
    gtk_label_set_line_wrap(GTK_LABEL(page->config_status), TRUE);
    gtk_misc_set_alignment(GTK_MISC(page->config_status), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(hbox), page->config_status, TRUE, TRUE, 5);

    page->apply_button = gtk_button_new_with_label("Apply");
    ctk_config_set_tooltip(page->ctk_config, page->apply_button,
                           __apply_help);
    g_signal_connect(G_OBJECT(page->apply_button), "clicked",
                     G_CALLBACK(apply_clicked), page);
    page->reset_button = gtk_button_new_with_label("Reset");
    ctk_config_set_tooltip(page->ctk_config, page->reset_button,
                           __reset_help);
    g_signal_connect(G_OBJECT(page->reset_button), "clicked",
                     G_CALLBACK(reset_clicked), page);
    gtk_box_pack_end(GTK_BOX(hbox), page->apply_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), page->reset_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 5);

    dc_backend_add_watch(page->backend, backend_changed, page);
}


static void build_color_controls(CtkWlDisplay *page, GtkWidget *vbox)
{
    GtkWidget *table, *hbox;
    gint row = 0;

    page->rgb_values = g_array_new(FALSE, FALSE, sizeof(gint));
    page->format_values = g_array_new(FALSE, FALSE, sizeof(gint));

    table = ctk_section_table_new(vbox, "Color Controls");
    page->controls_table = table;
    page->rgb_combo = ctk_combo_box_text_new();
    config_row(page, table, &row, "Color Range:", page->rgb_combo,
               __rgb_control_help);
    page->format_combo = ctk_combo_box_text_new();
    config_row(page, table, &row, "Color Space:", page->format_combo,
               __format_control_help);
    g_signal_connect(G_OBJECT(page->rgb_combo), "changed",
                     G_CALLBACK(controls_changed), page);
    g_signal_connect(G_OBJECT(page->format_combo), "changed",
                     G_CALLBACK(controls_changed), page);

    hbox = gtk_hbox_new(FALSE, 5);
    page->controls_status = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(page->controls_status), TRUE);
    gtk_misc_set_alignment(GTK_MISC(page->controls_status), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(hbox), page->controls_status, TRUE, TRUE, 5);
    page->controls_apply = gtk_button_new_with_label("Apply");
    ctk_config_set_tooltip(page->ctk_config, page->controls_apply,
                           __controls_apply_help);
    g_signal_connect(G_OBJECT(page->controls_apply), "clicked",
                     G_CALLBACK(controls_apply_clicked), page);
    page->controls_reset = gtk_button_new_with_label("Reset");
    ctk_config_set_tooltip(page->ctk_config, page->controls_reset,
                           __reset_help);
    g_signal_connect(G_OBJECT(page->controls_reset), "clicked",
                     G_CALLBACK(reset_clicked), page);
    gtk_box_pack_end(GTK_BOX(hbox), page->controls_apply, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), page->controls_reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 5);
}


static GtkWidget *tab_box(GtkWidget *notebook, const gchar *title)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 5);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);

    gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
                                        GTK_SHADOW_NONE);
    ctk_scrolled_window_add(GTK_SCROLLED_WINDOW(sw), vbox);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), sw,
                             gtk_label_new(title));
    return vbox;
}


/* ---- construction ------------------------------------------------------- */

GtkWidget *ctk_wl_display_new(const gchar *card, const gchar *connector,
                              const gchar *gpu_label, CtkConfig *ctk_config)
{
    GObject *object;
    CtkWlDisplay *page;
    DcConnector *c;
    GtkWidget *banner, *vbox, *cvbox, *notebook, *table, *view, *w, *hbox;
    GtkWidget *button;
    GtkCellRenderer *renderer;
    const gchar *tool;
    gchar *s;
    gint row;

    c = dc_kms_connector(card, connector);
    if (!c) {
        return NULL;
    }

    object = g_object_new(CTK_TYPE_WL_DISPLAY, NULL);
    page = CTK_WL_DISPLAY(object);
    page->ctk_config = ctk_config;
    page->card = g_strdup(card);
    page->connector = g_strdup(connector);
    page->key = g_strdup_printf("%s-%s", card, connector);
    page->title = dc_connector_title(c);
    page->gpu_label = g_strdup(gpu_label);
    g_signal_connect(object, "destroy", G_CALLBACK(page_destroyed), NULL);

    gtk_box_set_spacing(GTK_BOX(page), 5);
    banner = ctk_banner_image_new(BANNER_ARTWORK_BLANK);
    gtk_box_pack_start(GTK_BOX(page), banner, FALSE, FALSE, 0);

    /* Tabs as on the X display device page */
    notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(notebook), GTK_POS_TOP);
    gtk_box_pack_start(GTK_BOX(page), notebook, TRUE, TRUE, 0);
    vbox = tab_box(notebook, "Information");
    cvbox = tab_box(notebook, "Controls");

    page->backend = dc_backend();
    if (page->backend) {
        build_color_controls(page, cvbox);
        if (page->backend->get_gamma && page->backend->set_gamma) {
            page->cc_box = tab_box(notebook, "Color Correction");
        }
    }

    /* --- information --- */
    table = ctk_section_table_new(vbox, "Display Device Information");
    row = 0;
    info_row(page, table, &row, "Monitor:", c->monitor, NULL);
    if (c->vendor) {
        s = g_strdup_printf("%s (product %04X%s%.0d)", c->vendor,
                            c->product_code, c->year ? ", made " : "",
                            c->year);
        info_row(page, table, &row, "Manufacturer:", s, NULL);
        g_free(s);
    }
    if (c->serial) {
        info_row(page, table, &row, "Serial Number:", c->serial, NULL);
    }
    if (c->width_mm && c->height_mm) {
        gdouble diag = sqrt((gdouble) c->width_mm * c->width_mm +
                            (gdouble) c->height_mm * c->height_mm) / 25.4;
        s = g_strdup_printf("%u x %u mm (%.1f\")", c->width_mm, c->height_mm,
                            diag);
        info_row(page, table, &row, "Physical Size:", s, NULL);
        g_free(s);
    }
    info_row(page, table, &row, "DRM Connector:", page->key, NULL);
    info_row(page, table, &row, "GPU:", gpu_label, NULL);
    page->status_label = info_row(page, table, &row, "Status:", NULL,
                                  __status_help);
    page->mode_label = info_row(page, table, &row, "Current Mode:", NULL,
                                __mode_help);
    page->vrr_label = info_row(page, table, &row, "Variable Refresh Rate:",
                               NULL, __vrr_help);

    /* --- color signal, on the Controls tab --- */
    if (c->max_bpc >= 0 || c->colorspace || c->rgb_range ||
        c->color_format || c->hdr_metadata >= 0) {
        table = ctk_section_table_new(cvbox, "Current Signal");
        row = 0;
        if (c->rgb_range) {
            page->rgb_label = info_row(page, table, &row, "Color Range:",
                                       NULL, __rgb_help);
        }
        if (c->color_format) {
            page->format_label = info_row(page, table, &row, "Color Space:",
                                          NULL, __format_help);
        }
        if (c->max_bpc >= 0) {
            page->bpc_label = info_row(page, table, &row,
                                       "Maximum Color Depth (bpc):", NULL,
                                       __bpc_help);
        }
        if (c->colorspace) {
            page->colorspace_label = info_row(page, table, &row,
                                              "Colorimetry:", NULL,
                                              __colorspace_help);
        }
        if (c->hdr_metadata >= 0) {
            page->hdr_label = info_row(page, table, &row, "HDR:", NULL,
                                       __hdr_help);
        }
    }

    /* --- modes --- */
    ctk_section_table_new(vbox, "Supported Modes");
    page->mode_store = gtk_list_store_new(MODE_NUM_COLS, G_TYPE_STRING,
                                          G_TYPE_STRING);
    view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(page->mode_store));
    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(view), -1,
                                                "Resolution", renderer,
                                                "text", MODE_COL_RESOLUTION,
                                                NULL);
    renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(view), -1,
                                                "Refresh Rates", renderer,
                                                "text", MODE_COL_RATES, NULL);
    ctk_config_set_tooltip(ctk_config, view, __modes_help);
    w = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(w),
                                        GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(w), view);
    gtk_widget_set_size_request(w, -1, 180);
    gtk_box_pack_start(GTK_BOX(vbox), w, FALSE, FALSE, 5);

    /* --- configuration --- */
    page->kms_info = c;
    page->unique_name = connector_name_unique(card, connector);
    if (page->backend) {
        build_config_controls(page, vbox);
    }

    ctk_section_table_new(vbox, page->backend ? "Desktop Display Settings"
                                    : "Configuration");
    hbox = gtk_hbox_new(FALSE, 10);
    {
        const DcCompositor *comp = dc_compositor();
        s = g_strdup_printf("This display is configured by %s.",
                            comp && comp->desktop ? comp->desktop :
                            "the Wayland compositor");
        w = gtk_label_new(s);
        g_free(s);
    }
    gtk_misc_set_alignment(GTK_MISC(w), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(hbox), w, FALSE, FALSE, 5);
    tool = dc_desktop_display_settings_name();
    if (tool) {
        s = g_strdup_printf("Open %s", tool);
        button = gtk_button_new_with_label(s);
        g_free(s);
        ctk_config_set_tooltip(ctk_config, button, __settings_help);
        g_signal_connect(G_OBJECT(button), "clicked",
                         G_CALLBACK(settings_clicked), page);
        gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    gtk_widget_show_all(GTK_WIDGET(page));
    ctk_wl_display_refresh(page);
    load_config(page);

    return GTK_WIDGET(page);
}


GtkTextBuffer *ctk_wl_display_create_help(GtkTextTagTable *table,
                                          CtkWlDisplay *page)
{
    GtkTextIter i;
    GtkTextBuffer *b = gtk_text_buffer_new(table);

    gtk_text_buffer_get_iter_at_offset(b, &i, 0);

    ctk_help_title(b, &i, "%s Help", page->title);
    ctk_help_para(b, &i, "This page shows a display connected to %s.  The "
                  "desktop is running on Wayland, where the compositor owns "
                  "the display configuration, so the information here is "
                  "read from the kernel's mode setting (KMS) interface.  It "
                  "is the same for every compositor.",
                  page->gpu_label ? page->gpu_label : "a GPU");

    ctk_help_heading(b, &i, "Status");
    ctk_help_para(b, &i, "%s", __status_help);
    ctk_help_heading(b, &i, "Current Mode");
    ctk_help_para(b, &i, "%s", __mode_help);
    ctk_help_heading(b, &i, "Variable Refresh Rate");
    ctk_help_para(b, &i, "%s", __vrr_help);
    if (page->bpc_label) {
        ctk_help_heading(b, &i, "Maximum Color Depth");
        ctk_help_para(b, &i, "%s", __bpc_help);
    }
    if (page->controls_table) {
        ctk_help_heading(b, &i, "Color Controls");
        ctk_help_para(b, &i, "These are on the Controls tab, and are "
                      "applied through %s.", page->backend->name);
        ctk_help_term(b, &i, "Color Range");
        ctk_help_para(b, &i, "%s", __rgb_control_help);
        ctk_help_term(b, &i, "Color Space");
        ctk_help_para(b, &i, "%s", __format_control_help);
        ctk_help_term(b, &i, "Apply");
        ctk_help_para(b, &i, "%s", __controls_apply_help);
    }
    if (page->rgb_label) {
        ctk_help_heading(b, &i, "Current Color Range");
        ctk_help_para(b, &i, "%s", __rgb_help);
    }
    if (page->format_label) {
        ctk_help_heading(b, &i, "Current Color Space");
        ctk_help_para(b, &i, "%s", __format_help);
    }
    if (page->colorspace_label) {
        ctk_help_heading(b, &i, "Colorimetry");
        ctk_help_para(b, &i, "%s", __colorspace_help);
    }
    if (page->hdr_label) {
        ctk_help_heading(b, &i, "HDR");
        ctk_help_para(b, &i, "%s", __hdr_help);
    }
    ctk_help_heading(b, &i, "Supported Modes");
    ctk_help_para(b, &i, "%s", __modes_help);
    if (page->backend) {
        ctk_help_heading(b, &i, "Display Configuration");
        ctk_help_para(b, &i, "These settings are applied through %s.  "
                      "nvidia-settings has to use each desktop's own "
                      "interface, since Wayland has no common one: the "
                      "DisplayConfig D-Bus interface on GNOME and Cinnamon, "
                      "the KDE output management protocol on Plasma, and "
                      "the wlr output management protocol on sway, "
                      "Hyprland, niri and other wlroots-style compositors.",
                      page->backend->name);
        ctk_help_term(b, &i, "Display");
        ctk_help_para(b, &i, "%s", __enable_help);
        ctk_help_term(b, &i, "Resolution");
        ctk_help_para(b, &i, "%s", __resolution_help);
        ctk_help_term(b, &i, "Refresh Rate");
        ctk_help_para(b, &i, "%s", __rate_help);
        ctk_help_term(b, &i, "Scale");
        ctk_help_para(b, &i, "%s", __scale_help);
        ctk_help_term(b, &i, "Rotation");
        ctk_help_para(b, &i, "%s", __rotation_help);
        ctk_help_term(b, &i, "Position");
        ctk_help_para(b, &i, "%s", __position_help);
        if (page->backend->caps & DC_CAP_PRIMARY) {
            ctk_help_term(b, &i, "Primary");
            ctk_help_para(b, &i, "%s", __primary_help);
        }
        ctk_help_term(b, &i, "Variable Refresh Rate");
        ctk_help_para(b, &i, "%s", __vrr_policy_help);
        ctk_help_term(b, &i, "Apply");
        ctk_help_para(b, &i, "%s", __apply_help);
        ctk_help_term(b, &i, "Reset");
        ctk_help_para(b, &i, "%s", __reset_help);
    }
    if (page->cc_box) {
        ctk_wl_color_correction_help(b, &i);
    }
    ctk_help_heading(b, &i, "Desktop Display Settings");
    ctk_help_para(b, &i, "%s", __settings_help);

    ctk_help_finish(b);
    return b;
}
