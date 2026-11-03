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
"Full (0-255) or Limited (16-235) RGB range.";
static const char *__hdr_help =
"Whether the compositor is sending HDR metadata to the monitor, which "
"means HDR output is enabled.";
static const char *__modes_help =
"The modes the monitor advertises.  The current mode is marked with *, "
"the monitor's preferred mode with +.";
static const char *__settings_help =
"Under Wayland the compositor owns the display configuration.  Use the "
"desktop's display settings to change the resolution, refresh rate, "
"scaling, position or variable refresh rate of this display.";


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
        res = g_strdup_printf("%ux%u%s", first->width, first->height,
                              first->interlaced ? "i" : "");
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
}


/* ---- construction ------------------------------------------------------- */

GtkWidget *ctk_wl_display_new(const gchar *card, const gchar *connector,
                              const gchar *gpu_label, CtkConfig *ctk_config)
{
    GObject *object;
    CtkWlDisplay *page;
    DcConnector *c;
    GtkWidget *banner, *vbox, *sw, *table, *view, *w, *hbox, *button;
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

    /* --- color --- */
    if (c->max_bpc >= 0 || c->colorspace || c->rgb_range ||
        c->hdr_metadata >= 0) {
        table = ctk_section_table_new(vbox, "Color");
        row = 0;
        if (c->max_bpc >= 0) {
            page->bpc_label = info_row(page, table, &row,
                                       "Maximum Color Depth (bpc):", NULL,
                                       __bpc_help);
        }
        if (c->colorspace) {
            page->colorspace_label = info_row(page, table, &row,
                                              "Colorspace:", NULL,
                                              __colorspace_help);
        }
        if (c->rgb_range) {
            page->rgb_label = info_row(page, table, &row, "RGB Range:", NULL,
                                       __rgb_help);
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
    ctk_section_table_new(vbox, "Configuration");
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

    dc_connector_free(c);

    gtk_widget_show_all(GTK_WIDGET(page));
    ctk_wl_display_refresh(page);

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
    if (page->colorspace_label) {
        ctk_help_heading(b, &i, "Colorspace");
        ctk_help_para(b, &i, "%s", __colorspace_help);
    }
    if (page->rgb_label) {
        ctk_help_heading(b, &i, "RGB Range");
        ctk_help_para(b, &i, "%s", __rgb_help);
    }
    if (page->hdr_label) {
        ctk_help_heading(b, &i, "HDR");
        ctk_help_para(b, &i, "%s", __hdr_help);
    }
    ctk_help_heading(b, &i, "Supported Modes");
    ctk_help_para(b, &i, "%s", __modes_help);
    ctk_help_heading(b, &i, "Configuration");
    ctk_help_para(b, &i, "%s", __settings_help);

    ctk_help_finish(b);
    return b;
}
