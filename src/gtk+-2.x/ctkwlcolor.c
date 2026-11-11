/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * Color correction (brightness, contrast, gamma) of a display under
 * Wayland.  The ramps are computed as on the X Color Correction page and
 * handed to the compositor, which owns the CRTC gamma tables.
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

#include <gtk/gtk.h>

#include "NvCtrlAttributes.h"

#include "rgb_xpm.h"
#include "red_xpm.h"
#include "green_xpm.h"
#include "blue_xpm.h"

#include "ctkwlcolor.h"
#include "ctkcurve.h"
#include "ctkscale.h"
#include "ctkhelp.h"
#include "ctkutils.h"

/* Coalesce slider motion into one compositor call per this interval */
#define GAMMA_APPLY_DELAY_MS 30

static const char *__channel_help =
"Selects the color channel(s) the sliders below change.";
static const char *__curve_help =
"The color ramps sent to the display: each line maps an input level "
"(left to right) to the output level (bottom to top) of one channel.";
static const char *__brightness_help =
"The Brightness slider alters the amount of brightness for the selected "
"color channel(s).";
static const char *__contrast_help =
"The Contrast slider alters the amount of contrast for the selected color "
"channel(s).";
static const char *__gamma_help =
"The Gamma slider alters the amount of gamma for the selected color "
"channel(s).";
static const char *__reset_help =
"Restores the default brightness, contrast and gamma of all channels.";
static const char *__note_text =
"Applied immediately through the compositor.  These settings are not "
"saved: they end when you log out, and the desktop may replace them (for "
"example Night Light or a color profile).";

typedef struct {
    DcBackend *backend;
    gchar *output;

    GtkWidget *channel_combo;
    GtkWidget *curve;
    GtkWidget *status;
    GtkAdjustment *brightness;
    GtkAdjustment *contrast;
    GtkAdjustment *gamma;

    NvCtrlGammaInput input;
    guint size;
    gushort *ramp[3];

    gboolean updating;
    guint apply_id;
} WlColor;


static guint selected_channels(WlColor *wc)
{
    switch (gtk_combo_box_get_active(GTK_COMBO_BOX(wc->channel_combo))) {
    case 1:  return RED_CHANNEL;
    case 2:  return GREEN_CHANNEL;
    case 3:  return BLUE_CHANNEL;
    default: return ALL_CHANNELS;
    }
}


static gint first_channel(guint channels)
{
    gint ch;

    for (ch = FIRST_COLOR_CHANNEL; ch <= LAST_COLOR_CHANNEL; ch++) {
        if (channels & (1 << ch)) return ch;
    }
    return FIRST_COLOR_CHANNEL;
}


static void get_ramp(gpointer data, gint channel, gushort **lut, gint *n)
{
    WlColor *wc = data;
    gint ch = first_channel(channel);

    *lut = wc->ramp[ch];
    *n = wc->ramp[ch] ? (gint) wc->size : 0;
}


static void set_status(WlColor *wc, const gchar *text)
{
    gtk_label_set_text(GTK_LABEL(wc->status), text ? text : __note_text);
}


static gboolean apply_now(gpointer data)
{
    WlColor *wc = data;
    GError *error = NULL;

    wc->apply_id = 0;
    if (!wc->backend->set_gamma(wc->backend, wc->output, wc->size,
                                wc->ramp[RED_CHANNEL_INDEX],
                                wc->ramp[GREEN_CHANNEL_INDEX],
                                wc->ramp[BLUE_CHANNEL_INDEX], &error)) {
        gchar *s = g_strdup_printf("Unable to set the color correction: %s",
                                   error->message);
        set_status(wc, s);
        g_free(s);
        g_error_free(error);
    } else {
        set_status(wc, NULL);
    }
    return FALSE;
}


static void update_ramps(WlColor *wc)
{
    NvCtrlUpdateGammaRamp(&wc->input, wc->size, wc->ramp, ALL_CHANNELS);
    ctk_curve_color_changed(wc->curve);
    if (!wc->apply_id) {
        wc->apply_id = g_timeout_add(GAMMA_APPLY_DELAY_MS, apply_now, wc);
    }
}


/* Show the values of the selected channel(s) on the sliders */
static void load_sliders(WlColor *wc)
{
    gint ch = first_channel(selected_channels(wc));

    wc->updating = TRUE;
    gtk_adjustment_set_value(wc->brightness, wc->input.brightness[ch]);
    gtk_adjustment_set_value(wc->contrast, wc->input.contrast[ch]);
    gtk_adjustment_set_value(wc->gamma, wc->input.gamma[ch]);
    wc->updating = FALSE;
}


static void channel_changed(GtkComboBox *combo, gpointer data)
{
    load_sliders(data);
}


static void slider_changed(GtkAdjustment *adj, gpointer data)
{
    WlColor *wc = data;
    guint channels = selected_channels(wc);
    gfloat v[3];
    guint what = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(adj),
                                                    "color_attribute"));
    gint ch;

    if (wc->updating) return;

    for (ch = 0; ch < 3; ch++) {
        v[ch] = gtk_adjustment_get_value(adj);
    }
    NvCtrlAssignGammaInput(&wc->input, v, v, v, channels | what);
    update_ramps(wc);
}


static void reset_clicked(GtkButton *button, gpointer data)
{
    WlColor *wc = data;

    NvCtrlInitGammaInputStruct(&wc->input);
    load_sliders(wc);
    update_ramps(wc);
}


static void wl_color_free(gpointer data)
{
    WlColor *wc = data;
    gint ch;

    if (wc->apply_id) {
        g_source_remove(wc->apply_id);
    }
    for (ch = 0; ch < 3; ch++) {
        g_free(wc->ramp[ch]);
    }
    g_free(wc->output);
    g_free(wc);
}


static GtkAdjustment *new_adjustment(WlColor *wc, guint what, gdouble def,
                                     gdouble min, gdouble max)
{
    GtkAdjustment *adj = GTK_ADJUSTMENT(
        gtk_adjustment_new(def, min, max, (max - min) / 250.0,
                           (max - min) / 25.0, 0.0));

    g_object_set_data(G_OBJECT(adj), "color_attribute",
                      GUINT_TO_POINTER(what));
    g_signal_connect(G_OBJECT(adj), "value_changed",
                     G_CALLBACK(slider_changed), wc);
    return adj;
}


static void add_scale(WlColor *wc, GtkWidget *box, GtkAdjustment *adj,
                      const gchar *name, const gchar *help,
                      CtkConfig *ctk_config)
{
    GtkWidget *scale = ctk_scale_new(adj, name, ctk_config, G_TYPE_DOUBLE);

    gtk_box_pack_start(GTK_BOX(box), scale, TRUE, TRUE, 0);
    ctk_config_set_tooltip(ctk_config, CTK_SCALE(scale)->gtk_scale, help);
}


GtkWidget *ctk_wl_color_correction_new(DcBackend *backend,
                                       const gchar *output,
                                       CtkConfig *ctk_config)
{
    WlColor *wc;
    GtkWidget *top, *mainhbox, *leftvbox, *rightvbox, *vbox, *hbox;
    GtkWidget *label, *alignment, *eventbox, *button;
    GtkListStore *store;
    GtkTreeIter iter;
    GtkCellRenderer *renderer;
    GError *error = NULL;
    guint16 *r = NULL, *g = NULL, *b = NULL;
    guint size = 0;

    top = gtk_vbox_new(FALSE, 10);
    gtk_container_set_border_width(GTK_CONTAINER(top), 5);

    if (!backend || !backend->get_gamma || !backend->set_gamma ||
        !backend->get_gamma(backend, output, &size, &r, &g, &b, &error)) {
        gchar *s = g_strdup_printf("Color correction is not available for "
                                   "this display%s%s",
                                   error ? ": " : ".",
                                   error ? error->message : "");
        label = gtk_label_new(s);
        g_free(s);
        g_clear_error(&error);
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.0f);
        gtk_box_pack_start(GTK_BOX(top), label, FALSE, FALSE, 0);
        return top;
    }
    /* Only the ramp size is used: the sliders start from the defaults, and
     * nothing is sent until one of them moves */
    g_free(r);
    g_free(g);
    g_free(b);

    wc = g_new0(WlColor, 1);
    wc->backend = backend;
    wc->output = g_strdup(output);
    wc->size = size;
    wc->ramp[0] = g_new0(gushort, size);
    wc->ramp[1] = g_new0(gushort, size);
    wc->ramp[2] = g_new0(gushort, size);
    NvCtrlInitGammaInputStruct(&wc->input);
    NvCtrlUpdateGammaRamp(&wc->input, size, wc->ramp, ALL_CHANNELS);
    g_object_set_data_full(G_OBJECT(top), "wl-color", wc, wl_color_free);

    mainhbox = gtk_hbox_new(FALSE, 10);
    gtk_box_pack_start(GTK_BOX(top), mainhbox, FALSE, FALSE, 0);
    leftvbox = gtk_vbox_new(FALSE, 10);
    gtk_box_pack_start(GTK_BOX(mainhbox), leftvbox, FALSE, FALSE, 0);
    rightvbox = gtk_vbox_new(FALSE, 10);
    gtk_box_pack_start(GTK_BOX(mainhbox), rightvbox, TRUE, TRUE, 0);

    /* channel selection, as on the X Color Correction page */
    vbox = gtk_vbox_new(FALSE, 5);
    label = gtk_label_new("Active Color Channel:");
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(leftvbox), vbox, FALSE, FALSE, 0);

    store = gtk_list_store_new(2, GDK_TYPE_PIXBUF, G_TYPE_STRING);
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, 0,
                       gdk_pixbuf_new_from_xpm_data(rgb_xpm),
                       1, "All Channels", -1);
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, 0,
                       gdk_pixbuf_new_from_xpm_data(red_xpm), 1, "Red", -1);
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, 0,
                       gdk_pixbuf_new_from_xpm_data(green_xpm),
                       1, "Green", -1);
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, 0,
                       gdk_pixbuf_new_from_xpm_data(blue_xpm),
                       1, "Blue", -1);
    wc->channel_combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    renderer = gtk_cell_renderer_pixbuf_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(wc->channel_combo), renderer,
                               FALSE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(wc->channel_combo),
                                   renderer, "pixbuf", 0, NULL);
    renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(wc->channel_combo), renderer,
                               FALSE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(wc->channel_combo),
                                   renderer, "text", 1, NULL);
    gtk_combo_box_set_active(GTK_COMBO_BOX(wc->channel_combo), 0);
    gtk_box_pack_start(GTK_BOX(vbox), wc->channel_combo, FALSE, FALSE, 0);
    ctk_config_set_tooltip(ctk_config, wc->channel_combo, __channel_help);
    g_signal_connect(G_OBJECT(wc->channel_combo), "changed",
                     G_CALLBACK(channel_changed), wc);

    /* curve */
    alignment = gtk_alignment_new(0, 0, 1.0, 1.0);
    gtk_box_pack_start(GTK_BOX(leftvbox), alignment, TRUE, TRUE, 0);
    wc->curve = ctk_curve_new_with_ramps(get_ramp, wc);
    eventbox = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(eventbox), wc->curve);
    gtk_container_add(GTK_CONTAINER(alignment), eventbox);
    ctk_config_set_tooltip(ctk_config, eventbox, __curve_help);

    /* sliders */
    wc->brightness = new_adjustment(wc, BRIGHTNESS_VALUE, BRIGHTNESS_DEFAULT,
                                    BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    add_scale(wc, rightvbox, wc->brightness, "Brightness",
              __brightness_help, ctk_config);
    wc->contrast = new_adjustment(wc, CONTRAST_VALUE, CONTRAST_DEFAULT,
                                  CONTRAST_MIN, CONTRAST_MAX);
    add_scale(wc, rightvbox, wc->contrast, "Contrast", __contrast_help,
              ctk_config);
    wc->gamma = new_adjustment(wc, GAMMA_VALUE, GAMMA_DEFAULT, GAMMA_MIN,
                               GAMMA_MAX);
    add_scale(wc, rightvbox, wc->gamma, "Gamma", __gamma_help, ctk_config);

    gtk_box_pack_start(GTK_BOX(top), gtk_hseparator_new(), FALSE, FALSE, 0);

    /* note and reset */
    hbox = gtk_hbox_new(FALSE, 10);
    wc->status = gtk_label_new(NULL);
    set_status(wc, NULL);
    gtk_label_set_line_wrap(GTK_LABEL(wc->status), TRUE);
    gtk_misc_set_alignment(GTK_MISC(wc->status), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(hbox), wc->status, TRUE, TRUE, 0);
    button = gtk_button_new_with_label("Reset Hardware Defaults");
    ctk_config_set_tooltip(ctk_config, button, __reset_help);
    g_signal_connect(G_OBJECT(button), "clicked",
                     G_CALLBACK(reset_clicked), wc);
    gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), hbox, FALSE, FALSE, 0);

    return top;
}


void ctk_wl_color_correction_help(GtkTextBuffer *b, GtkTextIter *i)
{
    ctk_help_heading(b, i, "Color Correction");
    ctk_help_para(b, i, "Adjusts brightness, contrast and gamma of this "
                  "display, for all color channels or one at a time.  "
                  "%s", __note_text);
    ctk_help_heading(b, i, "Active Color Channel");
    ctk_help_para(b, i, "%s", __channel_help);
    ctk_help_heading(b, i, "Brightness, Contrast and Gamma");
    ctk_help_para(b, i, "%s  %s  %s", __brightness_help, __contrast_help,
                  __gamma_help);
    ctk_help_heading(b, i, "Reset Hardware Defaults");
    ctk_help_para(b, i, "%s", __reset_help);
}
