/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon GPU information and power management page.  All data comes
 * from the amdgpu kernel driver's sysfs interface, so this page works on
 * any display server (X11 or Wayland) and without NV-CONTROL.
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
#include <sys/utsname.h>

#include <gtk/gtk.h>

#include "ctkamdgpu.h"
#include "ctkbanner.h"
#include "ctkhelp.h"
#include "ctkutils.h"

#define AMD_GPU_UPDATE_INTERVAL_MS 1000

typedef struct _CtkAmdTempRow {
    GtkWidget *label;
    gchar *input_file;   /* e.g. "temp1_input" (relative to hwmon) */
} CtkAmdTempRow;

static const struct {
    const gchar *value;
    const gchar *label;
} perf_levels[] = {
    { "auto",             "Auto (driver managed)" },
    { "low",              "Low (lowest clocks)" },
    { "high",             "High (highest clocks)" },
    { "manual",           "Manual (choose clock levels)" },
    { "profile_standard", "Profiling: Standard" },
    { "profile_min_sclk", "Profiling: Minimum GPU clock" },
    { "profile_min_mclk", "Profiling: Minimum memory clock" },
    { "profile_peak",     "Profiling: Peak" },
};
#define NUM_PERF_LEVELS (sizeof(perf_levels) / sizeof(perf_levels[0]))


static const char *__perf_level_help =
"Selects the amdgpu DPM performance level "
"(power_dpm_force_performance_level).  'Auto' lets the driver pick clocks "
"based on load and is recommended for normal use.  'Low' and 'High' pin the "
"clocks to the lowest or highest level.  'Manual' allows choosing GPU and "
"memory clock levels below.  The profiling levels are intended for "
"benchmarking.  Changing this setting requires root privileges; you will "
"be prompted to authenticate through polkit.";

static const char *__power_profile_help =
"Selects the workload power profile (pp_power_profile_mode), which tunes "
"how aggressively the GPU raises and lowers its clocks.";

static const char *__power_cap_help =
"Sets the board power limit in watts (hwmon power1_cap).  The allowed range "
"is reported by the driver.";

static const char *__manual_clock_help =
"When the performance level is 'Manual', forces the GPU core (sclk) or "
"memory (mclk) clock to the selected DPM level.";

static const char *__status_help =
"Live readings from the amdgpu driver, refreshed once per second while this "
"page is visible.";


GType ctk_amd_gpu_get_type(void)
{
    static GType ctk_amd_gpu_type = 0;

    if (!ctk_amd_gpu_type) {
        static const GTypeInfo info = {
            sizeof (CtkAmdGpuClass),
            NULL, /* base_init */
            NULL, /* base_finalize */
            NULL, /* class_init */
            NULL, /* class_finalize */
            NULL, /* class_data */
            sizeof (CtkAmdGpu),
            0,    /* n_preallocs */
            NULL, /* instance_init */
            NULL  /* value_table */
        };

        ctk_amd_gpu_type =
            g_type_register_static(GTK_TYPE_VBOX, "CtkAmdGpu", &info, 0);
    }

    return ctk_amd_gpu_type;
}


/* ---- small helpers ------------------------------------------------------ */

static gchar *format_bytes(gint64 bytes)
{
    if (bytes >= ((gint64) 1 << 30)) {
        return g_strdup_printf("%.2f GiB", bytes / (double) (1 << 30));
    }
    return g_strdup_printf("%.0f MiB", bytes / (double) (1 << 20));
}


static gchar *format_usage(const gchar *dir, const gchar *used_file,
                           const gchar *total_file)
{
    gint64 used, total;
    gchar *u, *t, *ret;

    if (!amdgpu_read_int64(dir, used_file, &used) ||
        !amdgpu_read_int64(dir, total_file, &total) || total <= 0) {
        return NULL;
    }
    u = format_bytes(used);
    t = format_bytes(total);
    ret = g_strdup_printf("%s / %s (%.0f%%)", u, t, 100.0 * used / total);
    g_free(u);
    g_free(t);
    return ret;
}


static void set_label(GtkWidget *label, gchar *text /* takes ownership */)
{
    if (!label) {
        g_free(text);
        return;
    }
    gtk_label_set_text(GTK_LABEL(label), text ? text : "N/A");
    g_free(text);
}


static GtkWidget *add_row(CtkAmdGpu *ctk, GtkWidget *table, gint *row,
                          const gchar *name, gchar *value /* owned */,
                          const gchar *help)
{
    GtkWidget *label;

    gtk_table_resize(GTK_TABLE(table), *row + 1, 2);
    label = add_table_row_with_help_text(table, ctk->ctk_config, help, *row, 0,
                                         0.0f, 0.5f, name, 0.0f, 0.5f,
                                         value ? value : "N/A");
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
                     GTK_FILL, GTK_FILL, 0, 0);
    gtk_box_pack_start(GTK_BOX(hbox), control, FALSE, FALSE, 0);
    gtk_table_attach(GTK_TABLE(table), hbox, 1, 2, *row, *row + 1,
                     GTK_FILL, GTK_FILL, 0, 0);
    if (help) {
        ctk_config_set_tooltip(config, control, help);
    }
    (*row)++;
}


static void report_write(CtkAmdGpu *ctk, gboolean ok, gchar *err,
                         const gchar *what)
{
    if (ok) {
        ctk_config_statusbar_message(ctk->ctk_config, "%s updated.", what);
    } else {
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(ctk)), err);
    }
    g_free(err);
}


/* ---- live update -------------------------------------------------------- */

static void populate_clock_combo(CtkAmdGpu *ctk, GtkWidget *combo,
                                 const gchar *file);

static gboolean update_status(gpointer user_data)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(user_data);
    AmdGpu *gpu = ctk->gpu;
    const gchar *dev = gpu->device_path;
    const gchar *hw = gpu->hwmon_path;
    gint64 v, v2;
    GList *l;

    if (ctk->gpu_busy) {
        set_label(ctk->gpu_busy,
                  amdgpu_read_int64(dev, "gpu_busy_percent", &v) ?
                  g_strdup_printf("%" G_GINT64_FORMAT " %%", v) : NULL);
    }
    if (ctk->mem_busy) {
        set_label(ctk->mem_busy,
                  amdgpu_read_int64(dev, "mem_busy_percent", &v) ?
                  g_strdup_printf("%" G_GINT64_FORMAT " %%", v) : NULL);
    }
    if (ctk->vram_usage) {
        set_label(ctk->vram_usage,
                  format_usage(dev, "mem_info_vram_used",
                               "mem_info_vram_total"));
    }
    if (ctk->gtt_usage) {
        set_label(ctk->gtt_usage,
                  format_usage(dev, "mem_info_gtt_used", "mem_info_gtt_total"));
    }
    if (ctk->sclk) {
        gchar *s = NULL;
        if (hw && amdgpu_read_int64(hw, "freq1_input", &v)) {
            s = g_strdup_printf("%" G_GINT64_FORMAT " MHz", v / 1000000);
        } else {
            s = amdgpu_dpm_current_label(dev, "pp_dpm_sclk");
        }
        set_label(ctk->sclk, s);
    }
    if (ctk->mclk) {
        gchar *s = NULL;
        if (hw && amdgpu_read_int64(hw, "freq2_input", &v)) {
            s = g_strdup_printf("%" G_GINT64_FORMAT " MHz", v / 1000000);
        } else {
            s = amdgpu_dpm_current_label(dev, "pp_dpm_mclk");
        }
        set_label(ctk->mclk, s);
    }
    if (ctk->power && hw) {
        gboolean ok = amdgpu_read_int64(hw, "power1_average", &v) ||
                      amdgpu_read_int64(hw, "power1_input", &v);
        gchar *s = NULL;
        if (ok) {
            if (amdgpu_read_int64(hw, "power1_cap", &v2) && v2 > 0) {
                s = g_strdup_printf("%.1f W / %.0f W", v / 1e6, v2 / 1e6);
            } else {
                s = g_strdup_printf("%.1f W", v / 1e6);
            }
        }
        set_label(ctk->power, s);
    }
    if (ctk->voltage && hw) {
        set_label(ctk->voltage,
                  amdgpu_read_int64(hw, "in0_input", &v) ?
                  g_strdup_printf("%" G_GINT64_FORMAT " mV", v) : NULL);
    }
    if (ctk->fan && hw) {
        gchar *s = NULL;
        gboolean have_rpm = amdgpu_read_int64(hw, "fan1_input", &v);
        gboolean have_pwm = amdgpu_read_int64(hw, "pwm1", &v2);
        if (have_rpm && have_pwm) {
            s = g_strdup_printf("%" G_GINT64_FORMAT " RPM (%.0f%%)",
                                v, v2 * 100.0 / 255.0);
        } else if (have_rpm) {
            s = g_strdup_printf("%" G_GINT64_FORMAT " RPM", v);
        } else if (have_pwm) {
            s = g_strdup_printf("%.0f%%", v2 * 100.0 / 255.0);
        }
        set_label(ctk->fan, s);
    }
    for (l = ctk->temp_rows; l; l = l->next) {
        CtkAmdTempRow *row = l->data;
        set_label(row->label,
                  amdgpu_read_int64(hw, row->input_file, &v) ?
                  g_strdup_printf("%.1f \302\260C", v / 1000.0) : NULL);
    }

    if (ctk->perf_level_current) {
        set_label(ctk->perf_level_current,
                  amdgpu_read_string(dev, "power_dpm_force_performance_level"));
    }

    /* Keep the performance-level combo in sync with external changes */
    if (ctk->perf_level_combo) {
        gchar *cur = amdgpu_read_string(dev,
                                        "power_dpm_force_performance_level");
        guint i;
        if (cur) {
            for (i = 0; i < NUM_PERF_LEVELS; i++) {
                if (strcmp(cur, perf_levels[i].value) == 0) {
                    if (gtk_combo_box_get_active(
                            GTK_COMBO_BOX(ctk->perf_level_combo)) != (gint) i) {
                        ctk->updating = TRUE;
                        gtk_combo_box_set_active(
                            GTK_COMBO_BOX(ctk->perf_level_combo), i);
                        ctk->updating = FALSE;
                    }
                    break;
                }
            }
            if (ctk->manual_box) {
                gtk_widget_set_sensitive(ctk->manual_box,
                                         strcmp(cur, "manual") == 0);
            }
            g_free(cur);
        }
    }

    return TRUE;
}


/* ---- control callbacks -------------------------------------------------- */

static void perf_level_changed(GtkComboBox *combo, gpointer user_data)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(user_data);
    gint idx = gtk_combo_box_get_active(combo);
    gchar *err = NULL;
    gboolean ok;

    if (ctk->updating || idx < 0 || idx >= (gint) NUM_PERF_LEVELS) {
        return;
    }

    ok = amdgpu_write_string(ctk->gpu->device_path,
                             "power_dpm_force_performance_level",
                             perf_levels[idx].value, &err);
    report_write(ctk, ok, err, "Performance level");

    if (ok) {
        /* Clock tables change with the performance level */
        populate_clock_combo(ctk, ctk->sclk_combo, "pp_dpm_sclk");
        populate_clock_combo(ctk, ctk->mclk_combo, "pp_dpm_mclk");
    }
    update_status(ctk);
}


static void profile_changed(GtkComboBox *combo, gpointer user_data)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(user_data);
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
    CtkAmdGpu *ctk = CTK_AMD_GPU(user_data);
    gint watts =
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ctk->power_cap_spin));
    gchar *val = g_strdup_printf("%" G_GINT64_FORMAT, (gint64) watts * 1000000);
    gchar *err = NULL;
    gboolean ok = amdgpu_write_string(ctk->gpu->hwmon_path, "power1_cap", val,
                                      &err);
    g_free(val);
    report_write(ctk, ok, err, "Power limit");
    update_status(ctk);
}


static void power_cap_reset(GtkButton *button, gpointer user_data)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(user_data);
    gint64 def;

    if (amdgpu_read_int64(ctk->gpu->hwmon_path, "power1_cap_default", &def)) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ctk->power_cap_spin),
                                  def / 1000000);
        power_cap_apply(button, user_data);
    }
}


static void clock_level_changed(GtkComboBox *combo, gpointer user_data)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(user_data);
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
                 strcmp(file, "pp_dpm_sclk") == 0 ? "GPU clock level"
                                                  : "Memory clock level");
    update_status(ctk);
}


static void populate_clock_combo(CtkAmdGpu *ctk, GtkWidget *combo,
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
        gchar *text = g_strdup_printf("%d: %s", lvl->index, lvl->label);
        ctk_combo_box_text_append_text(combo, text);
        g_free(text);
        if (lvl->active && active < 0) {
            active = i;
        }
    }
    amdgpu_free_dpm_levels(levels);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    ctk->updating = FALSE;
}


/* ---- construction ------------------------------------------------------- */

static void build_info_section(CtkAmdGpu *ctk, GtkWidget *vbox)
{
    AmdGpu *gpu = ctk->gpu;
    const gchar *dev = gpu->device_path;
    GtkWidget *table = ctk_section_table_new(vbox, "Graphics Card Information");
    struct utsname uts;
    gint row = 0;
    gint64 v;
    gchar *s;

    add_row(ctk, table, &row, "Graphics Processor:",
            g_strdup(gpu->product_name), NULL);

    add_row(ctk, table, &row, "Kernel Driver:",
            uname(&uts) == 0 ?
            g_strdup_printf("amdgpu (Linux %s)", uts.release) :
            g_strdup("amdgpu"), NULL);

    add_row(ctk, table, &row, "DRM Device:",
            g_strdup_printf("/dev/dri/%s", gpu->card_name), NULL);

    s = amdgpu_read_string(dev, "vbios_version");
    if (s && s[0]) {
        add_row(ctk, table, &row, "VBIOS Version:", s, NULL);
    } else {
        g_free(s);
    }

    add_row(ctk, table, &row, "Bus Location:", g_strdup(gpu->pci_slot), NULL);
    add_row(ctk, table, &row, "PCI Device ID:",
            g_strdup_printf("%04x:%04x (rev %02x)", gpu->vendor_id,
                            gpu->device_id, gpu->revision), NULL);
    add_row(ctk, table, &row, "PCI Subsystem ID:",
            g_strdup_printf("%04x:%04x", gpu->subsys_vendor_id,
                            gpu->subsys_device_id), NULL);

    s = amdgpu_read_string(dev, "current_link_speed");
    if (s) {
        gchar *w = amdgpu_read_string(dev, "current_link_width");
        gchar *ms = amdgpu_read_string(dev, "max_link_speed");
        gchar *mw = amdgpu_read_string(dev, "max_link_width");
        add_row(ctk, table, &row, "PCIe Link:",
                g_strdup_printf("x%s @ %s (max x%s @ %s)",
                                w ? w : "?", s, mw ? mw : "?", ms ? ms : "?"),
                NULL);
        g_free(w);
        g_free(ms);
        g_free(mw);
        g_free(s);
    }

    if (amdgpu_read_int64(dev, "mem_info_vram_total", &v)) {
        gint64 vis;
        gchar *total = format_bytes(v);
        if (amdgpu_read_int64(dev, "mem_info_vis_vram_total", &vis) &&
            vis != v) {
            gchar *vs = format_bytes(vis);
            add_row(ctk, table, &row, "Video Memory:",
                    g_strdup_printf("%s (%s CPU visible)", total, vs), NULL);
            g_free(vs);
        } else {
            add_row(ctk, table, &row, "Video Memory:", g_strdup(total), NULL);
        }
        g_free(total);
    }
    if (amdgpu_read_int64(dev, "mem_info_gtt_total", &v)) {
        add_row(ctk, table, &row, "GTT (System) Memory:", format_bytes(v),
                NULL);
    }

    s = amdgpu_read_string(dev, "unique_id");
    if (s && s[0]) {
        add_row(ctk, table, &row, "Unique ID:", s, NULL);
    } else {
        g_free(s);
    }

    {
        GList *l;
        GString *outs = g_string_new(NULL);
        for (l = gpu->connectors; l; l = l->next) {
            AmdConnector *c = l->data;
            gchar *status = amdgpu_read_string(c->sysfs_path, "status");
            if (status && strcmp(status, "connected") == 0) {
                if (outs->len) g_string_append(outs, ", ");
                g_string_append(outs, c->name);
            }
            g_free(status);
        }
        add_row(ctk, table, &row, "Connected Displays:",
                outs->len ? g_string_free(outs, FALSE)
                          : (g_string_free(outs, TRUE), g_strdup("None")),
                NULL);
    }
}


static gint compare_temp_names(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(a, b);
}


static void build_status_section(CtkAmdGpu *ctk, GtkWidget *vbox)
{
    AmdGpu *gpu = ctk->gpu;
    const gchar *dev = gpu->device_path;
    const gchar *hw = gpu->hwmon_path;
    GtkWidget *table = ctk_section_table_new(vbox, "Live Status");
    gint row = 0;

    if (amdgpu_file_exists(dev, "gpu_busy_percent")) {
        ctk->gpu_busy = add_row(ctk, table, &row, "GPU Utilization:", NULL,
                                __status_help);
    }
    if (amdgpu_file_exists(dev, "mem_busy_percent")) {
        ctk->mem_busy = add_row(ctk, table, &row, "Memory Utilization:", NULL,
                                __status_help);
    }
    if (amdgpu_file_exists(dev, "mem_info_vram_used")) {
        ctk->vram_usage = add_row(ctk, table, &row, "Video Memory Used:", NULL,
                                  __status_help);
    }
    if (amdgpu_file_exists(dev, "mem_info_gtt_used")) {
        ctk->gtt_usage = add_row(ctk, table, &row, "GTT Memory Used:", NULL,
                                 __status_help);
    }
    if ((hw && amdgpu_file_exists(hw, "freq1_input")) ||
        amdgpu_file_exists(dev, "pp_dpm_sclk")) {
        ctk->sclk = add_row(ctk, table, &row, "GPU Clock:", NULL,
                            __status_help);
    }
    if ((hw && amdgpu_file_exists(hw, "freq2_input")) ||
        amdgpu_file_exists(dev, "pp_dpm_mclk")) {
        ctk->mclk = add_row(ctk, table, &row, "Memory Clock:", NULL,
                            __status_help);
    }

    if (hw) {
        GDir *dir = g_dir_open(hw, 0, NULL);
        GList *names = NULL, *l;
        const gchar *entry;

        if (dir) {
            while ((entry = g_dir_read_name(dir))) {
                if (g_str_has_prefix(entry, "temp") &&
                    g_str_has_suffix(entry, "_input")) {
                    names = g_list_insert_sorted(names, g_strdup(entry),
                                                 compare_temp_names);
                }
            }
            g_dir_close(dir);
        }
        for (l = names; l; l = l->next) {
            gchar *input = l->data;
            gchar *base = g_strndup(input, strlen(input) - strlen("_input"));
            gchar *label_file = g_strdup_printf("%s_label", base);
            gchar *lbl = amdgpu_read_string(hw, label_file);
            gchar *title;
            CtkAmdTempRow *trow = g_new0(CtkAmdTempRow, 1);

            if (lbl && lbl[0]) {
                lbl[0] = g_ascii_toupper(lbl[0]);
                title = g_strdup_printf("Temperature (%s):", lbl);
            } else {
                title = g_strdup("Temperature:");
            }
            trow->input_file = input;
            trow->label = add_row(ctk, table, &row, title, NULL, __status_help);
            ctk->temp_rows = g_list_append(ctk->temp_rows, trow);

            g_free(title);
            g_free(lbl);
            g_free(label_file);
            g_free(base);
        }
        g_list_free(names);   /* strings now owned by temp rows */

        if (amdgpu_file_exists(hw, "power1_average") ||
            amdgpu_file_exists(hw, "power1_input")) {
            ctk->power = add_row(ctk, table, &row, "Power Draw:", NULL,
                                 __status_help);
        }
        if (amdgpu_file_exists(hw, "in0_input")) {
            ctk->voltage = add_row(ctk, table, &row, "GPU Voltage:", NULL,
                                   __status_help);
        }
        if (amdgpu_file_exists(hw, "fan1_input") ||
            amdgpu_file_exists(hw, "pwm1")) {
            ctk->fan = add_row(ctk, table, &row, "Fan Speed:", NULL,
                               __status_help);
        }
    }

    if (amdgpu_file_exists(dev, "power_dpm_force_performance_level")) {
        ctk->perf_level_current =
            add_row(ctk, table, &row, "Performance Level:", NULL,
                    __status_help);
    }
}


static void build_power_section(CtkAmdGpu *ctk, GtkWidget *vbox)
{
    AmdGpu *gpu = ctk->gpu;
    const gchar *dev = gpu->device_path;
    const gchar *hw = gpu->hwmon_path;
    GtkWidget *table;
    gint row = 0;
    gboolean any = amdgpu_file_exists(dev, "power_dpm_force_performance_level")
        || amdgpu_file_exists(dev, "pp_power_profile_mode")
        || (hw && amdgpu_file_exists(hw, "power1_cap"));

    if (!any) {
        return;
    }

    table = ctk_section_table_new(vbox, "Power Management");

    if (amdgpu_file_exists(dev, "power_dpm_force_performance_level")) {
        guint i;
        ctk->perf_level_combo = ctk_combo_box_text_new();
        for (i = 0; i < NUM_PERF_LEVELS; i++) {
            ctk_combo_box_text_append_text(ctk->perf_level_combo,
                                           perf_levels[i].label);
        }
        attach_control(table, &row, "Performance Level:",
                       ctk->perf_level_combo, ctk->ctk_config,
                       __perf_level_help);
        g_signal_connect(G_OBJECT(ctk->perf_level_combo), "changed",
                         G_CALLBACK(perf_level_changed), ctk);
    }

    ctk->profiles = amdgpu_read_power_profiles(dev);
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
        GtkWidget *reset = gtk_button_new_with_label("Default");
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
        if (amdgpu_file_exists(hw, "power1_cap_default")) {
            gtk_box_pack_start(GTK_BOX(hbox), reset, FALSE, FALSE, 0);
            g_signal_connect(G_OBJECT(reset), "clicked",
                             G_CALLBACK(power_cap_reset), ctk);
        } else {
            gtk_widget_destroy(reset);
        }
        attach_control(table, &row, "Power Limit:", hbox, ctk->ctk_config,
                       __power_cap_help);
        g_signal_connect(G_OBJECT(apply), "clicked",
                         G_CALLBACK(power_cap_apply), ctk);
    }

    /* Manual clock levels */
    if (amdgpu_file_exists(dev, "pp_dpm_sclk") ||
        amdgpu_file_exists(dev, "pp_dpm_mclk")) {
        GtkWidget *mtable = gtk_table_new(1, 2, FALSE);
        gint mrow = 0;

        gtk_table_set_row_spacings(GTK_TABLE(mtable), 3);
        gtk_table_set_col_spacings(GTK_TABLE(mtable), 15);

        if (amdgpu_file_exists(dev, "pp_dpm_sclk")) {
            ctk->sclk_combo = ctk_combo_box_text_new();
            g_object_set_data(G_OBJECT(ctk->sclk_combo), "amd-file",
                              "pp_dpm_sclk");
            populate_clock_combo(ctk, ctk->sclk_combo, "pp_dpm_sclk");
            attach_control(mtable, &mrow, "GPU Clock Level:", ctk->sclk_combo,
                           ctk->ctk_config, __manual_clock_help);
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

    if (!amdgpu_file_writable(dev, "power_dpm_force_performance_level")) {
        GtkWidget *note = gtk_label_new(
            "Changing these settings requires root privileges; you will be "
            "asked to authenticate.");
        gtk_misc_set_alignment(GTK_MISC(note), 0.0f, 0.5f);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_table_resize(GTK_TABLE(table), row + 1, 2);
        gtk_table_attach(GTK_TABLE(table), note, 0, 2, row, row + 1,
                         GTK_FILL, GTK_FILL, 0, 5);
        row++;
    }
}


GtkWidget *ctk_amd_gpu_new(AmdGpu *gpu, CtkConfig *ctk_config)
{
    GObject *object;
    CtkAmdGpu *ctk;
    GtkWidget *banner, *vbox, *sw;
    gchar *timer_name;

    g_return_val_if_fail(gpu != NULL, NULL);

    object = g_object_new(CTK_TYPE_AMD_GPU, NULL);
    ctk = CTK_AMD_GPU(object);
    ctk->gpu = gpu;
    ctk->ctk_config = ctk_config;

    gtk_box_set_spacing(GTK_BOX(ctk), 5);

    banner = ctk_banner_image_new(BANNER_ARTWORK_BLANK);
    gtk_box_pack_start(GTK_BOX(ctk), banner, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);

    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
                                        GTK_SHADOW_NONE);
    ctk_scrolled_window_add(GTK_SCROLLED_WINDOW(sw), vbox);
    gtk_box_pack_start(GTK_BOX(ctk), sw, TRUE, TRUE, 0);

    build_info_section(ctk, vbox);
    build_status_section(ctk, vbox);
    build_power_section(ctk, vbox);

    timer_name = g_strdup_printf("AMD GPU %d Monitor", gpu->index);
    ctk_config_add_timer(ctk_config, AMD_GPU_UPDATE_INTERVAL_MS, timer_name,
                         (GSourceFunc) update_status, (gpointer) ctk);
    g_free(timer_name);

    update_status(ctk);
    gtk_widget_show_all(GTK_WIDGET(ctk));

    return GTK_WIDGET(ctk);
}


GtkTextBuffer *ctk_amd_gpu_create_help(GtkTextTagTable *table,
                                       CtkAmdGpu *ctk)
{
    GtkTextIter i;
    GtkTextBuffer *b = gtk_text_buffer_new(table);

    gtk_text_buffer_get_iter_at_offset(b, &i, 0);

    ctk_help_title(b, &i, "AMD GPU Help");
    ctk_help_para(b, &i, "This page shows information about an AMD Radeon "
                  "GPU driven by the open source amdgpu kernel driver, and "
                  "lets you adjust its power management settings.  All data "
                  "is read from the driver's sysfs interface under %s.",
                  ctk->gpu->device_path);

    ctk_help_heading(b, &i, "Graphics Card Information");
    ctk_help_para(b, &i, "Static information about the GPU: product name, "
                  "kernel driver, PCI identifiers, PCIe link, memory sizes "
                  "and the DRM connectors that currently have a display "
                  "attached.");

    ctk_help_heading(b, &i, "Live Status");
    ctk_help_para(b, &i, "%s", __status_help);

    if (ctk->perf_level_combo) {
        ctk_help_heading(b, &i, "Performance Level");
        ctk_help_para(b, &i, "%s", __perf_level_help);
    }
    if (ctk->profile_combo) {
        ctk_help_heading(b, &i, "Power Profile");
        ctk_help_para(b, &i, "%s", __power_profile_help);
    }
    if (ctk->power_cap_spin) {
        ctk_help_heading(b, &i, "Power Limit");
        ctk_help_para(b, &i, "%s", __power_cap_help);
    }
    if (ctk->manual_box) {
        ctk_help_heading(b, &i, "GPU / Memory Clock Level");
        ctk_help_para(b, &i, "%s", __manual_clock_help);
    }

    ctk_help_finish(b);
    return b;
}


void ctk_amd_gpu_start_timer(GtkWidget *widget)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(widget);
    update_status(ctk);
    ctk_config_start_timer(ctk->ctk_config, (GSourceFunc) update_status,
                           (gpointer) ctk);
}


void ctk_amd_gpu_stop_timer(GtkWidget *widget)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(widget);
    ctk_config_stop_timer(ctk->ctk_config, (GSourceFunc) update_status,
                          (gpointer) ctk);
}
