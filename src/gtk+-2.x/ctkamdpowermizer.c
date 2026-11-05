/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon GPU PowerMizer page.  The amdgpu equivalent of PowerMizer is
 * its DPM (dynamic power management) interface in sysfs: the clock level
 * tables (pp_dpm_*), the forced performance level, the workload power
 * profile and the hwmon power limit.  This page presents them with the
 * same sections as the NVIDIA PowerMizer page.
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

#include <gtk/gtk.h>

#include "ctkamdpowermizer.h"
#include "ctkbanner.h"
#include "ctkhelp.h"
#include "ctkutils.h"

#define AMD_POWERMIZER_UPDATE_INTERVAL_MS 1000

/* power_dpm_force_performance_level values, in menu order */
static const struct {
    const gchar *value;
    const gchar *label;
} modes[] = {
    { "auto",             "Auto" },
    { "low",              "Prefer Minimum Power" },
    { "high",             "Prefer Maximum Performance" },
    { "manual",           "Manual" },
    { "profile_standard", "Profiling: Standard" },
    { "profile_min_sclk", "Profiling: Minimum Graphics Clock" },
    { "profile_min_mclk", "Profiling: Minimum Memory Clock" },
    { "profile_peak",     "Profiling: Peak" },
};
#define NUM_MODES (sizeof(modes) / sizeof(modes[0]))

/* DPM clock tables shown as Performance Levels columns */
static const struct {
    const gchar *file;
    const gchar *title;
} clock_tables[] = {
    { "pp_dpm_sclk",   "Graphics Clock" },
    { "pp_dpm_mclk",   "Memory Clock" },
    { "pp_dpm_fclk",   "Fabric Clock" },
    { "pp_dpm_socclk", "SoC Clock" },
};
#define NUM_CLOCK_TABLES (sizeof(clock_tables) / sizeof(clock_tables[0]))


static const char *__gpu_clock_help =
"This indicates the current graphics clock frequency.";

static const char *__mem_clock_help =
"This indicates the current memory clock frequency.";

static const char *__power_draw_help =
"This indicates the current power usage of the GPU, in watts, as reported "
"by the amdgpu hwmon interface.";

static const char *__default_power_limit_help =
"This indicates the default power limit of the board.";

static const char *__max_power_limit_help =
"This indicates the highest power limit that can be set.";

static const char *__current_pcie_link_width_help =
"This is the current PCIe link width of the GPU, in number of lanes.";

static const char *__current_pcie_link_speed_help =
"This is the current PCIe link speed of the GPU, in gigatransfers per "
"second (GT/s).";

static const char *__performance_level_help =
"This indicates the current graphics clock performance level: the active "
"row of the Graphics Clock column in the Performance Levels table.";

static const char *__performance_levels_table_help =
"This indicates the DPM (dynamic power management) levels the driver can "
"switch between for each clock domain.  The level in use for each clock is "
"shown normally; the other levels are greyed out.  On amdgpu each clock "
"domain changes level independently.";

static const char *__mode_help =
"Selects the amdgpu performance level (power_dpm_force_performance_level).  "
"'Auto' lets the driver raise and lower the clocks with the load and is "
"recommended for normal use.  'Prefer Minimum Power' keeps all clocks at "
"their lowest level and 'Prefer Maximum Performance' at their highest.  "
"'Manual' lets you choose the graphics and memory clock levels below.  The "
"profiling levels are fixed clocks intended for benchmarking.  'Current "
"Mode' shows the level the driver reports as in effect.";

static const char *__power_profile_help =
"Selects the workload power profile (pp_power_profile_mode), which tunes "
"how aggressively the GPU raises and lowers its clocks for a kind of work "
"such as 3D games, video playback or compute.";

static const char *__power_cap_help =
"Sets the board power limit in watts (hwmon power1_cap).  The allowed range "
"is reported by the driver.";

static const char *__manual_clock_help =
"When the Preferred Mode is 'Manual', forces the graphics (sclk) or memory "
"(mclk) clock to the selected level.";

static const char *__root_note =
"Changing these settings requires root privileges; you will be asked to "
"authenticate.";


GType ctk_amd_powermizer_get_type(void)
{
    static GType ctk_amd_powermizer_type = 0;

    if (!ctk_amd_powermizer_type) {
        static const GTypeInfo info = {
            sizeof (CtkAmdPowermizerClass),
            NULL, /* base_init */
            NULL, /* base_finalize */
            NULL, /* class_init */
            NULL, /* class_finalize */
            NULL, /* class_data */
            sizeof (CtkAmdPowermizer),
            0,    /* n_preallocs */
            NULL, /* instance_init */
            NULL  /* value_table */
        };

        ctk_amd_powermizer_type =
            g_type_register_static(GTK_TYPE_VBOX, "CtkAmdPowermizer",
                                   &info, 0);
    }

    return ctk_amd_powermizer_type;
}


/* ---- helpers ------------------------------------------------------------ */

static void set_label(GtkWidget *label, gchar *text /* takes ownership */)
{
    if (label) {
        gtk_label_set_text(GTK_LABEL(label), text ? text : "N/A");
    }
    g_free(text);
}


static GtkWidget *add_row(CtkAmdPowermizer *ctk, GtkWidget *table, gint *row,
                          const gchar *name, gchar *value /* owned */,
                          const gchar *help)
{
    GtkWidget *label;

    gtk_table_resize(GTK_TABLE(table), *row + 1, 2);
    label = add_table_row_with_help_text(table, ctk->ctk_config, help, *row, 0,
                                         0.0f, 0.5f, name, 0.0f, 0.5f,
                                         value ? value : "");
    g_free(value);
    (*row)++;
    return label;
}


static void attach_control(GtkWidget *table, gint *row, const gchar *name,
                           GtkWidget *control, CtkConfig *config,
                           const gchar *help)
{
    GtkWidget *label = gtk_label_new(name);
    GtkWidget *hbox = gtk_hbox_new(FALSE, 5);

    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    gtk_table_resize(GTK_TABLE(table), *row + 1, 2);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, *row, *row + 1,
                     GTK_FILL, GTK_FILL, 5, 0);
    gtk_box_pack_start(GTK_BOX(hbox), control, FALSE, FALSE, 0);
    gtk_table_attach(GTK_TABLE(table), hbox, 1, 2, *row, *row + 1,
                     GTK_FILL, GTK_FILL, 5, 0);
    if (help) {
        ctk_config_set_tooltip(config, control, help);
    }
    (*row)++;
}


static void report_write(CtkAmdPowermizer *ctk, gboolean ok, gchar *err,
                         const gchar *what)
{
    if (ok) {
        ctk_config_statusbar_message(ctk->ctk_config, "%s updated.", what);
    } else {
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(ctk)), err);
    }
    g_free(err);
}


/* "600Mhz" -> "600 MHz"; anything else is returned unchanged. */
static gchar *format_clock(const gchar *label)
{
    gchar *end;
    glong mhz = strtol(label, &end, 10);

    if (end != label && g_ascii_strncasecmp(end, "mhz", 3) == 0) {
        return g_strdup_printf("%ld MHz", mhz);
    }
    return g_strdup(label);
}


/* "16.0 GT/s PCIe" -> "16.0 GT/s" */
static gchar *read_link_speed(const gchar *dev, const gchar *file)
{
    gchar *s = amdgpu_read_string(dev, file);
    gchar *p;

    if (s && (p = strstr(s, " PCIe"))) {
        *p = '\0';
    }
    return s;
}


static gint mode_index(const gchar *value)
{
    guint i;

    for (i = 0; value && i < NUM_MODES; i++) {
        if (strcmp(value, modes[i].value) == 0) {
            return i;
        }
    }
    return -1;
}


/* The index of the active level of a DPM clock table, or -1. */
static gint active_level(const gchar *dev, const gchar *file)
{
    GList *levels = amdgpu_read_dpm_levels(dev, file), *l;
    gint ret = -1;

    for (l = levels; l; l = l->next) {
        AmdDpmLevel *lvl = l->data;
        if (lvl->active) {
            ret = lvl->index;
            break;
        }
    }
    amdgpu_free_dpm_levels(levels);
    return ret;
}


/* ---- Performance Levels table ------------------------------------------- */

static void populate_clock_combo(CtkAmdPowermizer *ctk, GtkWidget *combo,
                                 const gchar *file);

/*
 * Rebuild the table, like the NVIDIA page: a "Performance Level" column and
 * one column per clock domain, with the levels not in use greyed out.  The
 * table is only rebuilt when the driver's level tables change.
 */
static void update_perf_level_table(CtkAmdPowermizer *ctk)
{
    const gchar *dev = ctk->gpu->device_path;
    GList **levels, *c;
    GString *snapshot = g_string_new(NULL);
    GtkWidget *table, *label;
    guint ncols = g_list_length(ctk->columns), col, nrows = 0, i;

    if (!ctk->levels_hbox || ncols == 0) {
        g_string_free(snapshot, TRUE);
        return;
    }

    levels = g_new0(GList *, ncols);
    for (c = ctk->columns, col = 0; c; c = c->next, col++) {
        GList *l;
        levels[col] = amdgpu_read_dpm_levels(dev, c->data);
        nrows = MAX(nrows, g_list_length(levels[col]));
        for (l = levels[col]; l; l = l->next) {
            AmdDpmLevel *lvl = l->data;
            g_string_append_printf(snapshot, "%d %s%s;", lvl->index,
                                   lvl->label, lvl->active ? "*" : "");
        }
        g_string_append_c(snapshot, '|');
    }

    if (g_strcmp0(snapshot->str, ctk->levels_snapshot) == 0) {
        goto done;
    }
    g_free(ctk->levels_snapshot);
    ctk->levels_snapshot = g_strdup(snapshot->str);

    ctk_empty_container(ctk->levels_hbox);

    table = gtk_table_new(nrows + 1, ncols + 1, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 3);
    gtk_table_set_col_spacings(GTK_TABLE(table), 15);
    gtk_container_set_border_width(GTK_CONTAINER(table), 5);
    gtk_box_pack_start(GTK_BOX(ctk->levels_hbox), table, FALSE, FALSE, 0);
    ctk_config_set_tooltip(ctk->ctk_config, table,
                           __performance_levels_table_help);

    label = gtk_label_new("Level");
    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1,
                     GTK_FILL, GTK_FILL | GTK_EXPAND, 5, 0);

    for (c = ctk->columns, col = 0; c; c = c->next, col++) {
        guint t;
        for (t = 0; t < NUM_CLOCK_TABLES; t++) {
            if (strcmp(c->data, clock_tables[t].file) == 0) {
                label = gtk_label_new(clock_tables[t].title);
                gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
                gtk_table_attach(GTK_TABLE(table), label,
                                 col + 1, col + 2, 0, 1,
                                 GTK_FILL, GTK_FILL | GTK_EXPAND, 5, 0);
            }
        }
    }

    for (i = 0; i < nrows; i++) {
        gboolean row_active = FALSE;
        gchar *s;

        for (col = 0; col < ncols; col++) {
            AmdDpmLevel *lvl = g_list_nth_data(levels[col], i);
            if (!lvl) {
                continue;
            }
            s = format_clock(lvl->label);
            label = gtk_label_new(s);
            g_free(s);
            gtk_widget_set_sensitive(label, lvl->active);
            gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
            gtk_table_attach(GTK_TABLE(table), label,
                             col + 1, col + 2, i + 1, i + 2,
                             GTK_FILL, GTK_FILL | GTK_EXPAND, 5, 0);
            /* the level number follows the graphics clock, like NVIDIA */
            if (col == 0) {
                row_active = lvl->active;
            }
        }

        s = g_strdup_printf("%u", i);
        label = gtk_label_new(s);
        g_free(s);
        gtk_widget_set_sensitive(label, row_active);
        gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, i + 1, i + 2,
                         GTK_FILL, GTK_FILL | GTK_EXPAND, 5, 0);
    }

    gtk_widget_show_all(ctk->levels_hbox);

    /* Keep the manual level menus on the levels in use */
    populate_clock_combo(ctk, ctk->sclk_combo, "pp_dpm_sclk");
    populate_clock_combo(ctk, ctk->mclk_combo, "pp_dpm_mclk");

done:
    for (col = 0; col < ncols; col++) {
        amdgpu_free_dpm_levels(levels[col]);
    }
    g_free(levels);
    g_string_free(snapshot, TRUE);
}


/* ---- live update -------------------------------------------------------- */

static void update_mode(CtkAmdPowermizer *ctk)
{
    gchar *cur = amdgpu_read_string(ctk->gpu->device_path,
                                    "power_dpm_force_performance_level");
    gint idx = mode_index(cur);

    if (ctk->mode_txt) {
        set_label(ctk->mode_txt,
                  idx >= 0 ? g_strdup(modes[idx].label) : g_strdup(cur));
    }
    if (ctk->mode_combo && idx >= 0 &&
        gtk_combo_box_get_active(GTK_COMBO_BOX(ctk->mode_combo)) != idx) {
        ctk->updating = TRUE;
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctk->mode_combo), idx);
        ctk->updating = FALSE;
    }
    if (ctk->manual_box) {
        gtk_widget_set_sensitive(ctk->manual_box,
                                 cur && strcmp(cur, "manual") == 0);
    }
    g_free(cur);
}


static gboolean update_powermizer_info(gpointer user_data)
{
    CtkAmdPowermizer *ctk = CTK_AMD_POWERMIZER(user_data);
    const gchar *dev = ctk->gpu->device_path;
    const gchar *hw = ctk->gpu->hwmon_path;
    gint64 v;

    if (ctk->gpu_clock) {
        gchar *s = NULL;
        if (hw && amdgpu_read_int64(hw, "freq1_input", &v)) {
            s = g_strdup_printf("%" G_GINT64_FORMAT " MHz", v / 1000000);
        } else {
            gchar *l = amdgpu_dpm_current_label(dev, "pp_dpm_sclk");
            s = l ? format_clock(l) : NULL;
            g_free(l);
        }
        set_label(ctk->gpu_clock, s);
    }
    if (ctk->mem_clock) {
        gchar *s = NULL;
        if (hw && amdgpu_read_int64(hw, "freq2_input", &v)) {
            s = g_strdup_printf("%" G_GINT64_FORMAT " MHz", v / 1000000);
        } else {
            gchar *l = amdgpu_dpm_current_label(dev, "pp_dpm_mclk");
            s = l ? format_clock(l) : NULL;
            g_free(l);
        }
        set_label(ctk->mem_clock, s);
    }
    if (ctk->power_draw) {
        set_label(ctk->power_draw,
                  (amdgpu_read_int64(hw, "power1_average", &v) ||
                   amdgpu_read_int64(hw, "power1_input", &v)) ?
                  g_strdup_printf("%.2f W", v / 1e6) : NULL);
    }
    if (ctk->link_width) {
        gchar *w = amdgpu_read_string(dev, "current_link_width");
        set_label(ctk->link_width, w ? g_strdup_printf("x%s", w) : NULL);
        g_free(w);
    }
    if (ctk->link_speed) {
        set_label(ctk->link_speed, read_link_speed(dev, "current_link_speed"));
    }
    if (ctk->perf_level) {
        gint lvl = active_level(dev, "pp_dpm_sclk");
        set_label(ctk->perf_level,
                  lvl >= 0 ? g_strdup_printf("%d", lvl) : NULL);
    }

    update_perf_level_table(ctk);
    update_mode(ctk);

    return TRUE;
}


/* ---- control callbacks -------------------------------------------------- */

static void mode_changed(GtkComboBox *combo, gpointer user_data)
{
    CtkAmdPowermizer *ctk = CTK_AMD_POWERMIZER(user_data);
    gint idx = gtk_combo_box_get_active(combo);
    gchar *err = NULL;
    gboolean ok;

    if (ctk->updating || idx < 0 || idx >= (gint) NUM_MODES) {
        return;
    }

    ok = amdgpu_write_string(ctk->gpu->device_path,
                             "power_dpm_force_performance_level",
                             modes[idx].value, &err);
    report_write(ctk, ok, err, "Preferred mode");

    if (ok) {
        /* The active clock levels change with the performance level */
        populate_clock_combo(ctk, ctk->sclk_combo, "pp_dpm_sclk");
        populate_clock_combo(ctk, ctk->mclk_combo, "pp_dpm_mclk");
    }
    update_powermizer_info(ctk);
}


static void profile_changed(GtkComboBox *combo, gpointer user_data)
{
    CtkAmdPowermizer *ctk = CTK_AMD_POWERMIZER(user_data);
    gint idx = gtk_combo_box_get_active(combo);
    AmdPowerProfile *prof;
    gchar *err = NULL, *val;
    gboolean ok;

    if (ctk->updating || idx < 0) {
        return;
    }
    prof = g_list_nth_data(ctk->profiles, idx);
    if (!prof) {
        return;
    }
    val = g_strdup_printf("%d", prof->index);
    ok = amdgpu_write_string(ctk->gpu->device_path, "pp_power_profile_mode",
                             val, &err);
    g_free(val);
    report_write(ctk, ok, err, "Power profile");
}


static void power_cap_apply(GtkButton *button, gpointer user_data)
{
    CtkAmdPowermizer *ctk = CTK_AMD_POWERMIZER(user_data);
    gint watts =
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctk->power_cap_spin));
    gchar *val = g_strdup_printf("%" G_GINT64_FORMAT, (gint64) watts * 1000000);
    gchar *err = NULL;
    gboolean ok = amdgpu_write_string(ctk->gpu->hwmon_path, "power1_cap", val,
                                      &err);
    g_free(val);
    report_write(ctk, ok, err, "Power limit");
    update_powermizer_info(ctk);
}


static void power_cap_reset(GtkButton *button, gpointer user_data)
{
    CtkAmdPowermizer *ctk = CTK_AMD_POWERMIZER(user_data);
    gint64 def;

    if (amdgpu_read_int64(ctk->gpu->hwmon_path, "power1_cap_default", &def)) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ctk->power_cap_spin),
                                  def / 1000000);
        power_cap_apply(button, user_data);
    }
}


static void clock_level_changed(GtkComboBox *combo, gpointer user_data)
{
    CtkAmdPowermizer *ctk = CTK_AMD_POWERMIZER(user_data);
    const gchar *file = g_object_get_data(G_OBJECT(combo), "amd-file");
    GList *levels;
    AmdDpmLevel *lvl;
    gint idx = gtk_combo_box_get_active(combo);
    gchar *err = NULL, *val;
    gboolean ok;

    if (ctk->updating || idx < 0 || !file) {
        return;
    }
    levels = amdgpu_read_dpm_levels(ctk->gpu->device_path, file);
    lvl = g_list_nth_data(levels, idx);
    if (!lvl) {
        amdgpu_free_dpm_levels(levels);
        return;
    }
    val = g_strdup_printf("%d", lvl->index);
    amdgpu_free_dpm_levels(levels);

    ok = amdgpu_write_string(ctk->gpu->device_path, file, val, &err);
    g_free(val);
    report_write(ctk, ok, err,
                 strcmp(file, "pp_dpm_sclk") == 0 ? "Graphics clock level"
                                                  : "Memory clock level");
    update_powermizer_info(ctk);
}


static void populate_clock_combo(CtkAmdPowermizer *ctk, GtkWidget *combo,
                                 const gchar *file)
{
    GList *levels, *l;
    gint i = 0, active = -1;
    GtkTreeModel *model;

    if (!combo) {
        return;
    }

    ctk->updating = TRUE;
    model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
    gtk_list_store_clear(GTK_LIST_STORE(model));

    levels = amdgpu_read_dpm_levels(ctk->gpu->device_path, file);
    for (l = levels; l; l = l->next, i++) {
        AmdDpmLevel *lvl = l->data;
        gchar *clock = format_clock(lvl->label);
        gchar *text = g_strdup_printf("%d: %s", lvl->index, clock);
        ctk_combo_box_text_append_text(combo, text);
        g_free(text);
        g_free(clock);
        if (lvl->active && active < 0) {
            active = i;
        }
    }
    amdgpu_free_dpm_levels(levels);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    ctk->updating = FALSE;
}


/* ---- construction ------------------------------------------------------- */

static void build_info_section(CtkAmdPowermizer *ctk, GtkWidget *vbox)
{
    const gchar *dev = ctk->gpu->device_path;
    const gchar *hw = ctk->gpu->hwmon_path;
    GtkWidget *table = ctk_section_table_new(vbox, "PowerMizer Information");
    gint row = 0;
    gint64 v;

    gtk_container_set_border_width(GTK_CONTAINER(table), 10);

    if ((hw && amdgpu_file_exists(hw, "freq1_input")) ||
        amdgpu_file_exists(dev, "pp_dpm_sclk")) {
        ctk->gpu_clock = add_row(ctk, table, &row, "Graphics Clock:", NULL,
                                 __gpu_clock_help);
    }
    if ((hw && amdgpu_file_exists(hw, "freq2_input")) ||
        amdgpu_file_exists(dev, "pp_dpm_mclk")) {
        ctk->mem_clock = add_row(ctk, table, &row, "Memory Clock:", NULL,
                                 __mem_clock_help);
    }

    if (hw && (amdgpu_file_exists(hw, "power1_average") ||
               amdgpu_file_exists(hw, "power1_input"))) {
        row += 3;   /* spacing, as on the NVIDIA page */
        ctk->power_draw = add_row(ctk, table, &row, "Power Draw:", NULL,
                                  __power_draw_help);
    }
    if (hw && amdgpu_read_int64(hw, "power1_cap_default", &v)) {
        add_row(ctk, table, &row, "Default Power Limit:",
                g_strdup_printf("%.2f W", v / 1e6),
                __default_power_limit_help);
    }
    if (hw && amdgpu_read_int64(hw, "power1_cap_max", &v)) {
        add_row(ctk, table, &row, "Max Power Limit:",
                g_strdup_printf("%.2f W", v / 1e6), __max_power_limit_help);
    }

    if (amdgpu_file_exists(dev, "current_link_width") ||
        amdgpu_file_exists(dev, "current_link_speed")) {
        row += 3;
    }
    if (amdgpu_file_exists(dev, "current_link_width")) {
        ctk->link_width = add_row(ctk, table, &row, "Current PCIe Link Width:",
                                  NULL, __current_pcie_link_width_help);
    }
    if (amdgpu_file_exists(dev, "current_link_speed")) {
        ctk->link_speed = add_row(ctk, table, &row, "Current PCIe Link Speed:",
                                  NULL, __current_pcie_link_speed_help);
    }

    if (amdgpu_file_exists(dev, "pp_dpm_sclk")) {
        row += 3;
        ctk->perf_level = add_row(ctk, table, &row, "Performance Level:", NULL,
                                  __performance_level_help);
    }
}


static void build_levels_section(CtkAmdPowermizer *ctk, GtkWidget *vbox)
{
    guint t;

    for (t = 0; t < NUM_CLOCK_TABLES; t++) {
        GList *levels = amdgpu_read_dpm_levels(ctk->gpu->device_path,
                                               clock_tables[t].file);
        if (levels) {
            ctk->columns = g_list_append(ctk->columns,
                                         (gpointer) clock_tables[t].file);
        }
        amdgpu_free_dpm_levels(levels);
    }
    if (!ctk->columns) {
        return;
    }

    ctk->levels_hbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctk_section_box_new(vbox, "Performance Levels")),
                       ctk->levels_hbox, FALSE, FALSE, 0);
}


static void build_settings_section(CtkAmdPowermizer *ctk, GtkWidget *vbox)
{
    const gchar *dev = ctk->gpu->device_path;
    const gchar *hw = ctk->gpu->hwmon_path;
    GtkWidget *box, *table, *label;
    gint row = 0;
    gboolean have_mode =
        amdgpu_file_exists(dev, "power_dpm_force_performance_level");

    ctk->profiles = amdgpu_read_power_profiles(dev);

    if (!have_mode && !ctk->profiles &&
        !(hw && amdgpu_file_exists(hw, "power1_cap"))) {
        return;
    }

    box = ctk_section_box_new(vbox, "PowerMizer Settings");

    /* Preferred Mode / Current Mode on one row, as on the NVIDIA page */
    if (have_mode) {
        guint i;

        table = gtk_table_new(1, 4, FALSE);
        gtk_table_set_row_spacings(GTK_TABLE(table), 3);
        gtk_table_set_col_spacings(GTK_TABLE(table), 0);
        gtk_box_pack_start(GTK_BOX(box), table, FALSE, FALSE, 0);

        label = gtk_label_new("Preferred Mode:");
        gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1,
                         GTK_FILL, GTK_FILL | GTK_EXPAND, 5, 0);

        ctk->mode_combo = ctk_combo_box_text_new();
        for (i = 0; i < NUM_MODES; i++) {
            ctk_combo_box_text_append_text(ctk->mode_combo, modes[i].label);
        }
        ctk_config_set_tooltip(ctk->ctk_config, ctk->mode_combo, __mode_help);
        gtk_table_attach(GTK_TABLE(table), ctk->mode_combo, 1, 2, 0, 1,
                         GTK_FILL, GTK_FILL | GTK_EXPAND, 5, 0);

        label = gtk_label_new("Current Mode:");
        gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
        gtk_table_attach(GTK_TABLE(table), label, 2, 3, 0, 1,
                         GTK_FILL, GTK_FILL | GTK_EXPAND, 5, 0);

        ctk->mode_txt = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(ctk->mode_txt), 0.0f, 0.5f);
        gtk_table_attach(GTK_TABLE(table), ctk->mode_txt, 3, 4, 0, 1,
                         GTK_FILL, GTK_FILL | GTK_EXPAND, 5, 0);

        update_mode(ctk);
        g_signal_connect(G_OBJECT(ctk->mode_combo), "changed",
                         G_CALLBACK(mode_changed), ctk);
    }

    table = gtk_table_new(1, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 3);
    gtk_table_set_col_spacings(GTK_TABLE(table), 0);
    gtk_box_pack_start(GTK_BOX(box), table, FALSE, FALSE, 0);

    if (ctk->profiles) {
        GList *l;
        gint i = 0, active = -1;

        ctk->profile_combo = ctk_combo_box_text_new();
        for (l = ctk->profiles; l; l = l->next, i++) {
            AmdPowerProfile *p = l->data;
            gchar **words = g_strsplit(p->name, "_", -1);
            gchar **w;
            gchar *pretty;
            for (w = words; *w; w++) {
                gchar *lower = g_ascii_strdown(*w, -1);
                if (lower[0]) lower[0] = g_ascii_toupper(lower[0]);
                g_free(*w);
                *w = lower;
            }
            pretty = g_strjoinv(" ", words);
            ctk_combo_box_text_append_text(ctk->profile_combo, pretty);
            g_free(pretty);
            g_strfreev(words);
            if (p->active) active = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(ctk->profile_combo), active);
        attach_control(table, &row, "Power Profile:", ctk->profile_combo,
                       ctk->ctk_config, __power_profile_help);
        g_signal_connect(G_OBJECT(ctk->profile_combo), "changed",
                         G_CALLBACK(profile_changed), ctk);
    }

    if (hw && amdgpu_file_exists(hw, "power1_cap")) {
        gint64 cap = 0, cmin = 0, cmax = 0;
        GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
        GtkWidget *apply = gtk_button_new_with_label("Apply");
        GtkWidget *unit = gtk_label_new("W");

        amdgpu_read_int64(hw, "power1_cap", &cap);
        if (!amdgpu_read_int64(hw, "power1_cap_min", &cmin)) cmin = 0;
        if (!amdgpu_read_int64(hw, "power1_cap_max", &cmax) || cmax <= cmin) {
            cmax = MAX(cap, cmin) * 2;
        }

        ctk->power_cap_spin =
            gtk_spin_button_new_with_range(cmin / 1000000, cmax / 1000000, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ctk->power_cap_spin),
                                  cap / 1000000);
        gtk_box_pack_start(GTK_BOX(hbox), ctk->power_cap_spin, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), unit, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), apply, FALSE, FALSE, 0);
        g_signal_connect(G_OBJECT(apply), "clicked",
                         G_CALLBACK(power_cap_apply), ctk);
        if (amdgpu_file_exists(hw, "power1_cap_default")) {
            GtkWidget *reset = gtk_button_new_with_label("Default");
            gtk_box_pack_start(GTK_BOX(hbox), reset, FALSE, FALSE, 0);
            g_signal_connect(G_OBJECT(reset), "clicked",
                             G_CALLBACK(power_cap_reset), ctk);
        }
        attach_control(table, &row, "Power Limit:", hbox, ctk->ctk_config,
                       __power_cap_help);
    }

    /* Manual clock levels, usable while the Preferred Mode is Manual */
    if (have_mode && (amdgpu_file_exists(dev, "pp_dpm_sclk") ||
                      amdgpu_file_exists(dev, "pp_dpm_mclk"))) {
        GtkWidget *mtable = gtk_table_new(1, 2, FALSE);
        gint mrow = 0;

        gtk_table_set_row_spacings(GTK_TABLE(mtable), 3);
        gtk_table_set_col_spacings(GTK_TABLE(mtable), 0);

        if (amdgpu_file_exists(dev, "pp_dpm_sclk")) {
            ctk->sclk_combo = ctk_combo_box_text_new();
            g_object_set_data(G_OBJECT(ctk->sclk_combo), "amd-file",
                              "pp_dpm_sclk");
            populate_clock_combo(ctk, ctk->sclk_combo, "pp_dpm_sclk");
            attach_control(mtable, &mrow, "Graphics Clock Level:",
                           ctk->sclk_combo, ctk->ctk_config,
                           __manual_clock_help);
            g_signal_connect(G_OBJECT(ctk->sclk_combo), "changed",
                             G_CALLBACK(clock_level_changed), ctk);
        }
        if (amdgpu_file_exists(dev, "pp_dpm_mclk")) {
            ctk->mclk_combo = ctk_combo_box_text_new();
            g_object_set_data(G_OBJECT(ctk->mclk_combo), "amd-file",
                              "pp_dpm_mclk");
            populate_clock_combo(ctk, ctk->mclk_combo, "pp_dpm_mclk");
            attach_control(mtable, &mrow, "Memory Clock Level:",
                           ctk->mclk_combo, ctk->ctk_config,
                           __manual_clock_help);
            g_signal_connect(G_OBJECT(ctk->mclk_combo), "changed",
                             G_CALLBACK(clock_level_changed), ctk);
        }

        ctk->manual_box = mtable;
        gtk_table_resize(GTK_TABLE(table), row + 1, 2);
        gtk_table_attach(GTK_TABLE(table), mtable, 0, 2, row, row + 1,
                         GTK_FILL, GTK_FILL, 0, 0);
        row++;
    }

    if (have_mode &&
        !amdgpu_file_writable(dev, "power_dpm_force_performance_level")) {
        GtkWidget *note = gtk_label_new(__root_note);
        gtk_misc_set_alignment(GTK_MISC(note), 0.0f, 0.5f);
        gtk_misc_set_padding(GTK_MISC(note), 5, 0);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 5);
    }
}


GtkWidget *ctk_amd_powermizer_new(AmdGpu *gpu, CtkConfig *ctk_config)
{
    GObject *object;
    CtkAmdPowermizer *ctk;
    GtkWidget *banner, *vbox;
    gchar *timer_name, *name;

    g_return_val_if_fail(gpu != NULL, NULL);

    if (!amdgpu_file_exists(gpu->device_path, "pp_dpm_sclk") &&
        !amdgpu_file_exists(gpu->device_path,
                            "power_dpm_force_performance_level")) {
        return NULL;
    }

    object = g_object_new(CTK_TYPE_AMD_POWERMIZER, NULL);
    ctk = CTK_AMD_POWERMIZER(object);
    ctk->gpu = gpu;
    ctk->ctk_config = ctk_config;

    gtk_box_set_spacing(GTK_BOX(ctk), 5);

    banner = ctk_banner_image_new(BANNER_ARTWORK_THERMAL);
    gtk_box_pack_start(GTK_BOX(ctk), banner, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(ctk), vbox, TRUE, TRUE, 0);

    build_info_section(ctk, vbox);
    build_levels_section(ctk, vbox);
    build_settings_section(ctk, vbox);

    name = drm_gpu_name(gpu->card_name);
    timer_name = g_strdup_printf("PowerMizer Monitor (%s)", name);
    ctk_config_add_timer(ctk_config, AMD_POWERMIZER_UPDATE_INTERVAL_MS,
                         timer_name, (GSourceFunc) update_powermizer_info,
                         (gpointer) ctk);
    g_free(timer_name);
    g_free(name);

    update_powermizer_info(ctk);
    gtk_widget_show_all(GTK_WIDGET(ctk));

    return GTK_WIDGET(ctk);
}


GtkTextBuffer *ctk_amd_powermizer_create_help(GtkTextTagTable *table,
                                              CtkAmdPowermizer *ctk)
{
    GtkTextIter i;
    GtkTextBuffer *b = gtk_text_buffer_new(table);

    gtk_text_buffer_get_iter_at_offset(b, &i, 0);

    ctk_help_title(b, &i, "PowerMizer");
    ctk_help_para(b, &i, "This page shows the clock frequencies, power "
                  "usage and power management levels of an AMD GPU, and "
                  "lets you change its power management settings.  The "
                  "amdgpu kernel driver's equivalent of PowerMizer is its "
                  "DPM (dynamic power management) interface, under %s.",
                  ctk->gpu->device_path);

    ctk_help_heading(b, &i, "PowerMizer Information");
    if (ctk->gpu_clock) {
        ctk_help_term(b, &i, "Graphics Clock");
        ctk_help_para(b, &i, "%s", __gpu_clock_help);
    }
    if (ctk->mem_clock) {
        ctk_help_term(b, &i, "Memory Clock");
        ctk_help_para(b, &i, "%s", __mem_clock_help);
    }
    if (ctk->power_draw) {
        ctk_help_term(b, &i, "Power Draw");
        ctk_help_para(b, &i, "%s", __power_draw_help);
    }
    if (ctk->link_width) {
        ctk_help_term(b, &i, "Current PCIe Link Width");
        ctk_help_para(b, &i, "%s", __current_pcie_link_width_help);
    }
    if (ctk->link_speed) {
        ctk_help_term(b, &i, "Current PCIe Link Speed");
        ctk_help_para(b, &i, "%s", __current_pcie_link_speed_help);
    }
    if (ctk->perf_level) {
        ctk_help_term(b, &i, "Performance Level");
        ctk_help_para(b, &i, "%s", __performance_level_help);
    }

    if (ctk->levels_hbox) {
        ctk_help_heading(b, &i, "Performance Levels");
        ctk_help_para(b, &i, "%s", __performance_levels_table_help);
    }

    if (ctk->mode_combo || ctk->profile_combo || ctk->power_cap_spin) {
        ctk_help_heading(b, &i, "PowerMizer Settings");
    }
    if (ctk->mode_combo) {
        ctk_help_term(b, &i, "Preferred Mode");
        ctk_help_para(b, &i, "%s", __mode_help);
    }
    if (ctk->profile_combo) {
        ctk_help_term(b, &i, "Power Profile");
        ctk_help_para(b, &i, "%s", __power_profile_help);
    }
    if (ctk->power_cap_spin) {
        ctk_help_term(b, &i, "Power Limit");
        ctk_help_para(b, &i, "%s", __power_cap_help);
    }
    if (ctk->manual_box) {
        ctk_help_term(b, &i, "Graphics / Memory Clock Level");
        ctk_help_para(b, &i, "%s", __manual_clock_help);
    }
    if (ctk->mode_combo) {
        ctk_help_para(b, &i, "%s", __root_note);
    }

    ctk_help_finish(b);
    return b;
}


void ctk_amd_powermizer_start_timer(GtkWidget *widget)
{
    CtkAmdPowermizer *ctk = CTK_AMD_POWERMIZER(widget);

    update_powermizer_info(ctk);
    ctk_config_start_timer(ctk->ctk_config,
                           (GSourceFunc) update_powermizer_info,
                           (gpointer) ctk);
}


void ctk_amd_powermizer_stop_timer(GtkWidget *widget)
{
    CtkAmdPowermizer *ctk = CTK_AMD_POWERMIZER(widget);

    ctk_config_stop_timer(ctk->ctk_config,
                          (GSourceFunc) update_powermizer_info,
                          (gpointer) ctk);
}
