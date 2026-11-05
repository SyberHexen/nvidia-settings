/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon GPU thermal page: temperature sensors and fans, read from the
 * amdgpu driver's hwmon interface.  Laid out like the NVIDIA Thermal
 * Settings page.
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

#include "ctkamdthermal.h"
#include "ctkbanner.h"
#include "ctkgauge.h"
#include "ctkscale.h"
#include "ctkhelp.h"
#include "ctkutils.h"

#define AMD_THERMAL_UPDATE_INTERVAL_MS 1000
#define FRAME_PADDING 10

/* hwmon pwm values are 0-255 unless pwmN_max says otherwise */
#define AMD_PWM_MAX 255

/* pwmN_enable values */
#define AMD_PWM_FULL_SPEED 0
#define AMD_PWM_MANUAL     1
#define AMD_PWM_AUTO       2

typedef struct _CtkAmdSensor {
    gchar *input_file;      /* "temp1_input", relative to hwmon */
    GtkWidget *temp_label;
    GtkWidget *gauge;
} CtkAmdSensor;

typedef struct _CtkAmdFan {
    gint index;             /* N in fanN_* / pwmN_* */
    gint pwm_max;
    GtkWidget *rpm_label;
    GtkWidget *speed_label;
    GtkWidget *mode_label;
    GtkAdjustment *adjustment;   /* NULL without pwm control */
    GtkWidget *scale;
} CtkAmdFan;


static const char *__sensor_help =
"The thermal sensor, as named by the amdgpu driver: 'edge' is the GPU die "
"edge temperature, 'junction' the hottest point on the die (hotspot) and "
"'mem' the video memory.";

static const char *__provider_help =
"Where the reading comes from: the amdgpu driver's hwmon interface.";

static const char *__slowdown_help =
"The temperature at which the GPU starts to reduce its clocks to cool down "
"(hwmon tempN_crit).";

static const char *__shutdown_help =
"The temperature at which the GPU shuts down to protect itself "
"(hwmon tempN_emergency).";

static const char *__reading_help =
"The current temperature reported by this sensor.";

static const char *__gauge_help =
"The current temperature, on a scale up to the sensor's slowdown "
"temperature.";

static const char *__fan_id_help =
"The fan's number, as used by the amdgpu hwmon files (fanN, pwmN).";

static const char *__fan_rpm_help =
"The fan's current speed in rotations per minute (hwmon fanN_input).";

static const char *__fan_speed_help =
"The fan's current duty cycle as a percentage (hwmon pwmN).";

static const char *__fan_mode_help =
"Who controls the fan: 'Automatic' means the GPU firmware follows its own "
"fan curve, 'Manual' means the fan runs at the speed set below, and 'Full "
"speed' means fan control is disabled and the fan runs at maximum.";

static const char *__fan_range_help =
"The fan's minimum and maximum speed in RPM (hwmon fanN_min, fanN_max).";

static const char *__enable_help =
"The Enable GPU Fan Settings checkbox enables the fan speed sliders.  The "
"new speeds take effect when you press Apply, which switches the fans to "
"manual control (hwmon pwmN_enable = 1).  Changing fan settings requires "
"root privileges; you will be asked to authenticate.";

static const char *__apply_help =
"The Apply button switches the fans to manual control and sets them to the "
"speeds selected above.";

static const char *__reset_help =
"The Reset Hardware Defaults button gives fan control back to the GPU "
"firmware (hwmon pwmN_enable = 2).";

static const char *__fan_curve_help =
"On newer AMD GPUs (RDNA3 and later) the fans follow a curve of "
"temperature and speed points instead of the pwm interface.  This shows "
"the driver's current curve (gpu_od/fan_ctrl/fan_curve).";


GType ctk_amd_thermal_get_type(void)
{
    static GType ctk_amd_thermal_type = 0;

    if (!ctk_amd_thermal_type) {
        static const GTypeInfo info = {
            sizeof (CtkAmdThermalClass),
            NULL, /* base_init */
            NULL, /* base_finalize */
            NULL, /* class_init */
            NULL, /* class_finalize */
            NULL, /* class_data */
            sizeof (CtkAmdThermal),
            0,    /* n_preallocs */
            NULL, /* instance_init */
            NULL  /* value_table */
        };

        ctk_amd_thermal_type =
            g_type_register_static(GTK_TYPE_VBOX, "CtkAmdThermal", &info, 0);
    }

    return ctk_amd_thermal_type;
}



static void set_label(GtkWidget *label, gchar *text /* takes ownership */)
{
    if (label) {
        gtk_label_set_text(GTK_LABEL(label), text ? text : "N/A");
    }
    g_free(text);
}


static gchar *hwmon_file(const gchar *prefix, gint index, const gchar *suffix)
{
    return g_strdup_printf("%s%d_%s", prefix, index, suffix);
}


static gboolean read_hwmon(CtkAmdThermal *ctk, const gchar *prefix,
                           gint index, const gchar *suffix, gint64 *out)
{
    gchar *file = hwmon_file(prefix, index, suffix);
    gboolean ok = amdgpu_read_int64(ctk->gpu->hwmon_path, file, out);
    g_free(file);
    return ok;
}


static gboolean hwmon_has(CtkAmdThermal *ctk, const gchar *prefix,
                          gint index, const gchar *suffix)
{
    gchar *file = hwmon_file(prefix, index, suffix);
    gboolean ok = amdgpu_file_exists(ctk->gpu->hwmon_path, file);
    g_free(file);
    return ok;
}


/* "pwmN" itself has no suffix */
static gboolean read_pwm(CtkAmdThermal *ctk, gint index, gint64 *out)
{
    gchar *file = g_strdup_printf("pwm%d", index);
    gboolean ok = amdgpu_read_int64(ctk->gpu->hwmon_path, file, out);
    g_free(file);
    return ok;
}


static gchar *format_temp(gint64 millidegrees)
{
    return g_strdup_printf("%.0f\302\260C", millidegrees / 1000.0);
}


static gint compare_ints(gconstpointer a, gconstpointer b)
{
    return GPOINTER_TO_INT(a) - GPOINTER_TO_INT(b);
}


/*
 * The indices N for which "<prefix>N_<suffix>" exists in the hwmon
 * directory, in ascending order.
 */
static GList *hwmon_indices(const gchar *hwmon, const gchar *prefix,
                            const gchar *suffix)
{
    GDir *dir = g_dir_open(hwmon, 0, NULL);
    GList *indices = NULL;
    const gchar *entry;
    gsize plen = strlen(prefix);

    if (!dir) {
        return NULL;
    }
    while ((entry = g_dir_read_name(dir))) {
        gchar *end = NULL;
        gint64 n;

        if (!g_str_has_prefix(entry, prefix) ||
            !g_ascii_isdigit(entry[plen])) {
            continue;
        }
        n = g_ascii_strtoll(entry + plen, &end, 10);
        if (end && end[0] == '_' && strcmp(end + 1, suffix) == 0) {
            indices = g_list_insert_sorted(indices, GINT_TO_POINTER((gint) n),
                                           compare_ints);
        }
    }
    g_dir_close(dir);
    return indices;
}


/*
 * The fan curve points from gpu_od/fan_ctrl/fan_curve, one per line:
 * "OD_FAN_CURVE:\n0: 25C 15%\n1: 45C 30%\n...\nOD_RANGE:\n...".
 */
static gchar *read_fan_curve(const gchar *device_path)
{
    gchar *path = g_build_filename(device_path, "gpu_od", "fan_ctrl",
                                   "fan_curve", NULL);
    gchar *contents = NULL;
    GString *out;
    gchar **lines, **line;
    gboolean in_curve = FALSE;

    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        g_free(path);
        return NULL;
    }
    g_free(path);

    out = g_string_new(NULL);
    lines = g_strsplit(contents, "\n", -1);
    for (line = lines; *line; line++) {
        gint idx, temp, pct;

        if (g_str_has_prefix(*line, "OD_FAN_CURVE")) {
            in_curve = TRUE;
            continue;
        }
        if (g_str_has_prefix(*line, "OD_")) {
            in_curve = FALSE;
            continue;
        }
        if (in_curve &&
            sscanf(*line, "%d: %dC %d%%", &idx, &temp, &pct) == 3) {
            g_string_append_printf(out, "%s%d\302\260C \342\206\222 %d%%",
                                   out->len ? ",  " : "", temp, pct);
        }
    }
    g_strfreev(lines);
    g_free(contents);

    return g_string_free(out, out->len == 0);
}


static const gchar *pwm_mode_name(gint64 mode)
{
    switch (mode) {
    case AMD_PWM_FULL_SPEED: return "Full speed";
    case AMD_PWM_MANUAL:     return "Manual";
    case AMD_PWM_AUTO:       return "Automatic";
    default:                 return "Unknown";
    }
}


static gboolean fans_in_manual_mode(CtkAmdThermal *ctk)
{
    GList *l;

    for (l = ctk->fans; l; l = l->next) {
        CtkAmdFan *fan = l->data;
        gint64 mode;

        if (fan->adjustment &&
            read_hwmon(ctk, "pwm", fan->index, "enable", &mode) &&
            mode == AMD_PWM_MANUAL) {
            return TRUE;
        }
    }
    return FALSE;
}


static void set_controls_sensitive(CtkAmdThermal *ctk)
{
    gboolean enabled = ctk->enable_checkbox &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctk->enable_checkbox));
    GList *l;

    for (l = ctk->fans; l; l = l->next) {
        CtkAmdFan *fan = l->data;
        if (fan->scale) {
            gtk_widget_set_sensitive(fan->scale, enabled);
        }
    }
    if (ctk->apply_button) {
        gtk_widget_set_sensitive(ctk->apply_button,
                                 enabled && ctk->fan_values_changed);
    }
    if (ctk->reset_button) {
        gtk_widget_set_sensitive(ctk->reset_button,
                                 enabled || fans_in_manual_mode(ctk));
    }
}


/* ---- live update -------------------------------------------------------- */

static gboolean update_thermal(gpointer user_data)
{
    CtkAmdThermal *ctk = CTK_AMD_THERMAL(user_data);
    const gchar *hw = ctk->gpu->hwmon_path;
    GList *l;
    gint64 v;

    for (l = ctk->sensors; l; l = l->next) {
        CtkAmdSensor *sensor = l->data;

        if (amdgpu_read_int64(hw, sensor->input_file, &v)) {
            set_label(sensor->temp_label, format_temp(v));
            ctk_gauge_set_current(CTK_GAUGE(sensor->gauge),
                                  (gint) (v / 1000));
        } else {
            set_label(sensor->temp_label, NULL);
            ctk_gauge_set_current(CTK_GAUGE(sensor->gauge), 0);
        }
        ctk_gauge_draw(CTK_GAUGE(sensor->gauge));
    }

    for (l = ctk->fans; l; l = l->next) {
        CtkAmdFan *fan = l->data;
        gint64 pwm;

        set_label(fan->rpm_label,
                  read_hwmon(ctk, "fan", fan->index, "input", &v) ?
                  g_strdup_printf("%" G_GINT64_FORMAT, v) : NULL);

        if (read_pwm(ctk, fan->index, &pwm)) {
            set_label(fan->speed_label,
                      g_strdup_printf("%.0f", pwm * 100.0 / fan->pwm_max));
        } else {
            set_label(fan->speed_label, NULL);
        }

        set_label(fan->mode_label,
                  read_hwmon(ctk, "pwm", fan->index, "enable", &v) ?
                  g_strdup(pwm_mode_name(v)) : NULL);
    }

    if (ctk->fan_curve_label) {
        set_label(ctk->fan_curve_label,
                  read_fan_curve(ctk->gpu->device_path));
    }

    if (ctk->have_fan_control) {
        set_controls_sensitive(ctk);
    }

    return TRUE;
}


/* ---- fan control -------------------------------------------------------- */

static void report_write(CtkAmdThermal *ctk, gboolean ok, gchar *err,
                         const gchar *what)
{
    if (ok) {
        ctk_config_statusbar_message(ctk->ctk_config, "%s.", what);
    } else {
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(ctk)), err);
    }
    g_free(err);
}


static void enable_toggled(GtkToggleButton *button, gpointer user_data)
{
    CtkAmdThermal *ctk = CTK_AMD_THERMAL(user_data);

    if (!ctk->updating) {
        set_controls_sensitive(ctk);
    }
}


static void fan_value_changed(GtkAdjustment *adjustment, gpointer user_data)
{
    CtkAmdThermal *ctk = CTK_AMD_THERMAL(user_data);

    if (!ctk->updating) {
        ctk->fan_values_changed = TRUE;
        set_controls_sensitive(ctk);
    }
}


/* Write "<file>=<value>" for every controllable fan in one batch. */
static gboolean write_fans(CtkAmdThermal *ctk, gboolean manual,
                           gchar **error_msg)
{
    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
    gboolean ok;
    GList *l;

    for (l = ctk->fans; l; l = l->next) {
        CtkAmdFan *fan = l->data;

        if (!fan->adjustment) {
            continue;
        }
        g_ptr_array_add(files, hwmon_file("pwm", fan->index, "enable"));
        g_ptr_array_add(values, g_strdup_printf("%d", manual ?
                                                AMD_PWM_MANUAL :
                                                AMD_PWM_AUTO));
        if (manual) {
            gdouble pct = gtk_adjustment_get_value(fan->adjustment);
            g_ptr_array_add(files, g_strdup_printf("pwm%d", fan->index));
            g_ptr_array_add(values, g_strdup_printf("%d",
                (gint) (pct * fan->pwm_max / 100.0 + 0.5)));
        }
    }

    ok = amdgpu_write_strings(ctk->gpu->hwmon_path,
                              (const gchar * const *) files->pdata,
                              (const gchar * const *) values->pdata,
                              files->len, error_msg);

    g_ptr_array_free(files, TRUE);
    g_ptr_array_free(values, TRUE);
    return ok;
}


static void apply_clicked(GtkButton *button, gpointer user_data)
{
    CtkAmdThermal *ctk = CTK_AMD_THERMAL(user_data);
    gchar *err = NULL;
    gboolean ok = write_fans(ctk, TRUE, &err);

    if (ok) {
        ctk->fan_values_changed = FALSE;
    }
    report_write(ctk, ok, err, "Set the GPU fans to manual control");
    update_thermal(ctk);
}


static void sync_sliders(CtkAmdThermal *ctk)
{
    GList *l;

    ctk->updating = TRUE;
    for (l = ctk->fans; l; l = l->next) {
        CtkAmdFan *fan = l->data;
        gint64 pwm;

        if (fan->adjustment && read_pwm(ctk, fan->index, &pwm)) {
            gtk_adjustment_set_value(fan->adjustment,
                                     pwm * 100.0 / fan->pwm_max);
        }
    }
    ctk->updating = FALSE;
    ctk->fan_values_changed = FALSE;
}


static void reset_clicked(GtkButton *button, gpointer user_data)
{
    CtkAmdThermal *ctk = CTK_AMD_THERMAL(user_data);
    gchar *err = NULL;
    gboolean ok = write_fans(ctk, FALSE, &err);

    if (ok) {
        ctk->updating = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctk->enable_checkbox),
                                     FALSE);
        ctk->updating = FALSE;
        sync_sliders(ctk);
    }
    report_write(ctk, ok, err,
                 "Gave GPU fan control back to the firmware");
    update_thermal(ctk);
}


/* ---- page construction -------------------------------------------------- */

static GtkWidget *framed_value(GtkWidget *table, CtkConfig *config,
                               const gchar *name, gint row,
                               const gchar *help)
{
    GtkWidget *label = gtk_label_new(name);
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *eventbox = gtk_event_box_new();
    GtkWidget *value = gtk_label_new(NULL);

    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    gtk_table_attach(GTK_TABLE(table), label, 0, 1, row, row + 1,
                     GTK_FILL, GTK_FILL | GTK_EXPAND, 0, 0);
    gtk_container_add(GTK_CONTAINER(eventbox), frame);
    gtk_container_add(GTK_CONTAINER(frame), value);
    gtk_table_attach(GTK_TABLE(table), eventbox, 1, 2, row, row + 1,
                     GTK_FILL, GTK_FILL | GTK_EXPAND, 0, 0);
    ctk_config_set_tooltip(config, eventbox, help);
    return value;
}


static GtkWidget *pack_gauge(GtkWidget *hbox, gint lower, gint upper,
                             CtkConfig *config)
{
    GtkWidget *vbox = gtk_vbox_new(FALSE, 0);
    GtkWidget *frame = gtk_frame_new("Temperature");
    GtkWidget *inner = gtk_hbox_new(FALSE, 0);
    GtkWidget *eventbox = gtk_event_box_new();
    GtkWidget *gauge = ctk_gauge_new(lower, upper);

    gtk_box_pack_start(GTK_BOX(hbox), vbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(inner), FRAME_PADDING);
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_container_add(GTK_CONTAINER(eventbox), gauge);
    gtk_box_pack_start(GTK_BOX(inner), eventbox, FALSE, FALSE, 0);
    ctk_config_set_tooltip(config, eventbox, __gauge_help);
    return gauge;
}


static void build_sensor(CtkAmdThermal *ctk, GtkWidget *vbox, gint index)
{
    const gchar *hw = ctk->gpu->hwmon_path;
    CtkAmdSensor *sensor = g_new0(CtkAmdSensor, 1);
    GtkWidget *hbox, *info, *table;
    gchar *file, *name, *s;
    gint64 crit = 0, emergency = 0;
    gint row = 0, upper;

    sensor->input_file = hwmon_file("temp", index, "input");

    file = hwmon_file("temp", index, "label");
    name = amdgpu_read_string(hw, file);
    g_free(file);
    if (!name || !name[0]) {
        g_free(name);
        name = g_strdup_printf("temp%d", index);
    }

    hbox = gtk_hbox_new(FALSE, FRAME_PADDING);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 10);
    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    info = gtk_vbox_new(FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), info, FALSE, FALSE, 0);

    s = g_strdup_printf("ID: %d", index - 1);
    {
        GtkWidget *id = gtk_label_new(s);
        GtkWidget *idbox = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(idbox), id, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(info), idbox, FALSE, FALSE, 0);
    }
    g_free(s);

    table = gtk_table_new(1, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 3);
    gtk_table_set_col_spacings(GTK_TABLE(table), 15);
    gtk_container_set_border_width(GTK_CONTAINER(table), 5);
    gtk_box_pack_start(GTK_BOX(info), table, FALSE, FALSE, 0);

    {
        const gchar *what =
            !strcmp(name, "edge")     ? "GPU (edge)" :
            !strcmp(name, "junction") ? "GPU (junction / hotspot)" :
            !strcmp(name, "mem")      ? "Video memory" : name;
        gtk_table_resize(GTK_TABLE(table), row + 1, 2);
        add_table_row_with_help_text(table, ctk->ctk_config, __sensor_help,
                                     row++, 0, 0, 0.5, "Target:", 0, 0.5,
                                     what);
    }
    gtk_table_resize(GTK_TABLE(table), row + 1, 2);
    add_table_row_with_help_text(table, ctk->ctk_config, __provider_help,
                                 row++, 0, 0, 0.5, "Provider:", 0, 0.5,
                                 "amdgpu (hwmon)");

    if (read_hwmon(ctk, "temp", index, "crit", &crit) && crit > 0) {
        s = format_temp(crit);
        gtk_table_resize(GTK_TABLE(table), row + 1, 2);
        add_table_row_with_help_text(table, ctk->ctk_config, __slowdown_help,
                                     row++, 0, 0, 0.5, "Slowdown Temp:",
                                     0, 0.5, s);
        g_free(s);
    }
    if (read_hwmon(ctk, "temp", index, "emergency", &emergency) &&
        emergency > 0) {
        s = format_temp(emergency);
        gtk_table_resize(GTK_TABLE(table), row + 1, 2);
        add_table_row_with_help_text(table, ctk->ctk_config, __shutdown_help,
                                     row++, 0, 0, 0.5, "Shutdown Temp:",
                                     0, 0.5, s);
        g_free(s);
    }

    gtk_table_resize(GTK_TABLE(table), row + 1, 2);
    sensor->temp_label = framed_value(table, ctk->ctk_config, "Temperature:",
                                      row++, __reading_help);

    /* The gauge runs up to the slowdown temperature when there is one. */
    upper = crit > 0 ? (gint) (crit / 1000) : 100;
    sensor->gauge = pack_gauge(hbox, 25, upper, ctk->ctk_config);

    ctk->sensors = g_list_append(ctk->sensors, sensor);
    g_free(name);
}


static void fan_table_cell(GtkWidget *table, gint col, gint row,
                           GtkWidget *widget, CtkConfig *config,
                           const gchar *help)
{
    GtkWidget *eventbox = gtk_event_box_new();

    gtk_misc_set_alignment(GTK_MISC(widget), 0.0f, 0.5f);
    gtk_container_add(GTK_CONTAINER(eventbox), widget);
    gtk_table_attach(GTK_TABLE(table), eventbox, col, col + 1, row, row + 1,
                     GTK_FILL, GTK_FILL, 0, 0);
    if (help) {
        ctk_config_set_tooltip(config, eventbox, help);
    }
}


static void build_fans(CtkAmdThermal *ctk, GtkWidget *page_vbox)
{
    const gchar *hw = ctk->gpu->hwmon_path;
    GList *rpm = hwmon_indices(hw, "fan", "input");
    GList *pwm = hwmon_indices(hw, "pwm", "enable");
    GList *indices = NULL, *l;
    GtkWidget *vbox, *table, *control_vbox = NULL;
    gboolean have_range = FALSE;
    gint row, col;
    static const struct {
        const gchar *title;
        const gchar **help;
    } columns[] = {
        { "ID",            &__fan_id_help    },
        { "Speed (RPM)",   &__fan_rpm_help   },
        { "Speed (%)",     &__fan_speed_help },
        { "Control Type",  &__fan_mode_help  },
        { "Range (RPM)",   &__fan_range_help },
    };

    /* Every fan that has a speed reading or pwm control */
    for (l = rpm; l; l = l->next) {
        indices = g_list_insert_sorted(indices, l->data, compare_ints);
    }
    for (l = pwm; l; l = l->next) {
        if (!g_list_find(indices, l->data)) {
            indices = g_list_insert_sorted(indices, l->data, compare_ints);
        }
    }

    if (amdgpu_file_exists(ctk->gpu->device_path,
                           "gpu_od/fan_ctrl/fan_curve")) {
        GtkWidget *t;

        t = ctk_section_table_new(page_vbox, "Fan Curve");
        gtk_table_resize(GTK_TABLE(t), 1, 2);
        ctk->fan_curve_label =
            add_table_row_with_help_text(t, ctk->ctk_config,
                                         __fan_curve_help, 0, 0, 0, 0.5,
                                         "Temperature \342\206\222 Speed:",
                                         0, 0.5, NULL);
    }

    if (!indices) {
        g_list_free(rpm);
        g_list_free(pwm);
        return;
    }

    vbox = ctk_section_box_new(page_vbox, "Fan Information");

    for (l = indices; l; l = l->next) {
        if (hwmon_has(ctk, "fan", GPOINTER_TO_INT(l->data), "max")) {
            have_range = TRUE;
        }
    }

    table = gtk_table_new(1, have_range ? 5 : 4, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 3);
    gtk_table_set_col_spacings(GTK_TABLE(table), 15);
    gtk_container_set_border_width(GTK_CONTAINER(table), 5);
    gtk_box_pack_start(GTK_BOX(vbox), table, FALSE, FALSE, 0);

    for (col = 0; col < (have_range ? 5 : 4); col++) {
        fan_table_cell(table, col, 0, gtk_label_new(columns[col].title),
                       ctk->ctk_config, *columns[col].help);
    }

    row = 1;
    for (l = indices; l; l = l->next, row++) {
        CtkAmdFan *fan = g_new0(CtkAmdFan, 1);
        gint64 v, lo, hi;
        gchar *s;

        fan->index = GPOINTER_TO_INT(l->data);
        fan->pwm_max = (read_hwmon(ctk, "pwm", fan->index, "max", &v) &&
                        v > 0) ? (gint) v : AMD_PWM_MAX;

        gtk_table_resize(GTK_TABLE(table), row + 1, have_range ? 5 : 4);

        s = g_strdup_printf("%d", fan->index - 1);
        fan_table_cell(table, 0, row, gtk_label_new(s), ctk->ctk_config,
                       __fan_id_help);
        g_free(s);

        fan->rpm_label = gtk_label_new(NULL);
        fan_table_cell(table, 1, row, fan->rpm_label, ctk->ctk_config,
                       __fan_rpm_help);
        fan->speed_label = gtk_label_new(NULL);
        fan_table_cell(table, 2, row, fan->speed_label, ctk->ctk_config,
                       __fan_speed_help);
        fan->mode_label = gtk_label_new(NULL);
        fan_table_cell(table, 3, row, fan->mode_label, ctk->ctk_config,
                       __fan_mode_help);

        if (have_range) {
            s = (read_hwmon(ctk, "fan", fan->index, "min", &lo) &&
                 read_hwmon(ctk, "fan", fan->index, "max", &hi)) ?
                g_strdup_printf("%" G_GINT64_FORMAT " - %" G_GINT64_FORMAT,
                                lo, hi) : g_strdup("N/A");
            fan_table_cell(table, 4, row, gtk_label_new(s), ctk->ctk_config,
                           __fan_range_help);
            g_free(s);
        }

        /* Manual control needs both pwmN and pwmN_enable */
        if (hwmon_has(ctk, "pwm", fan->index, "enable") &&
            read_pwm(ctk, fan->index, &v)) {
            gchar *name = g_strdup_printf("Fan %d Speed", fan->index - 1);

            if (!control_vbox) {
                control_vbox = gtk_vbox_new(FALSE, 0);
                gtk_container_set_border_width(GTK_CONTAINER(control_vbox), 5);
            }
            fan->adjustment = GTK_ADJUSTMENT(
                gtk_adjustment_new(v * 100.0 / fan->pwm_max, 0, 100, 1, 5, 0));
            fan->scale = ctk_scale_new(fan->adjustment, name,
                                       ctk->ctk_config, G_TYPE_INT);
            gtk_box_pack_start(GTK_BOX(control_vbox), fan->scale,
                               FALSE, FALSE, 5);
            g_signal_connect(G_OBJECT(fan->adjustment), "value_changed",
                             G_CALLBACK(fan_value_changed), ctk);
            g_free(name);
            ctk->have_fan_control = TRUE;
        }

        ctk->fans = g_list_append(ctk->fans, fan);
    }

    if (ctk->have_fan_control) {
        GtkWidget *frame = gtk_frame_new(NULL);
        GtkWidget *fvbox = gtk_vbox_new(FALSE, 0);
        GtkWidget *bbox = gtk_hbox_new(FALSE, 0);

        ctk->enable_checkbox =
            gtk_check_button_new_with_label("Enable GPU Fan Settings");
        ctk_config_set_tooltip(ctk->ctk_config, ctk->enable_checkbox,
                               __enable_help);
        ctk->updating = TRUE;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctk->enable_checkbox),
                                     fans_in_manual_mode(ctk));
        ctk->updating = FALSE;
        g_signal_connect(G_OBJECT(ctk->enable_checkbox), "toggled",
                         G_CALLBACK(enable_toggled), ctk);

        ctk->apply_button = gtk_button_new_with_label("Apply");
        ctk_config_set_tooltip(ctk->ctk_config, ctk->apply_button,
                               __apply_help);
        g_signal_connect(G_OBJECT(ctk->apply_button), "clicked",
                         G_CALLBACK(apply_clicked), ctk);

        ctk->reset_button =
            gtk_button_new_with_label("Reset Hardware Defaults");
        ctk_config_set_tooltip(ctk->ctk_config, ctk->reset_button,
                               __reset_help);
        g_signal_connect(G_OBJECT(ctk->reset_button), "clicked",
                         G_CALLBACK(reset_clicked), ctk);

        gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 5);
        gtk_container_add(GTK_CONTAINER(frame), fvbox);
        gtk_box_pack_start(GTK_BOX(fvbox), ctk->enable_checkbox,
                           FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(fvbox), control_vbox, FALSE, FALSE, 0);

        gtk_box_pack_end(GTK_BOX(bbox), ctk->reset_button, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(bbox), ctk->apply_button, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), bbox, FALSE, FALSE, 5);

        set_controls_sensitive(ctk);
    } else if (control_vbox) {
        gtk_widget_destroy(control_vbox);
    }

    g_list_free(indices);
    g_list_free(rpm);
    g_list_free(pwm);
}


GtkWidget *ctk_amd_thermal_new(AmdGpu *gpu, CtkConfig *ctk_config)
{
    GObject *object;
    CtkAmdThermal *ctk;
    GtkWidget *banner, *vbox, *sw, *sensors_vbox;
    GList *temps, *l;
    gchar *timer_name;
    gboolean have_curve;

    g_return_val_if_fail(gpu != NULL, NULL);

    if (!gpu->hwmon_path) {
        return NULL;
    }

    temps = hwmon_indices(gpu->hwmon_path, "temp", "input");
    have_curve = amdgpu_file_exists(gpu->device_path,
                                    "gpu_od/fan_ctrl/fan_curve");
    {
        GList *fans = hwmon_indices(gpu->hwmon_path, "fan", "input");
        GList *pwms = hwmon_indices(gpu->hwmon_path, "pwm", "enable");
        gboolean any = temps || fans || pwms || have_curve;
        g_list_free(fans);
        g_list_free(pwms);
        if (!any) {
            return NULL;
        }
    }

    object = g_object_new(CTK_TYPE_AMD_THERMAL, NULL);
    ctk = CTK_AMD_THERMAL(object);
    ctk->gpu = gpu;
    ctk->ctk_config = ctk_config;

    gtk_box_set_spacing(GTK_BOX(ctk), 5);

    banner = ctk_banner_image_new(BANNER_ARTWORK_THERMAL);
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

    if (temps) {
        sensors_vbox = ctk_section_box_new(vbox, "Thermal Sensor Information");
        for (l = temps; l; l = l->next) {
            if (l != temps) {
                gtk_box_pack_start(GTK_BOX(sensors_vbox), gtk_hseparator_new(),
                                   FALSE, FALSE, 0);
            }
            build_sensor(ctk, sensors_vbox, GPOINTER_TO_INT(l->data));
        }
    }
    g_list_free(temps);

    build_fans(ctk, vbox);

    {
        gchar *name = drm_gpu_name(gpu->card_name);
        timer_name = g_strdup_printf("%s (AMD) Thermal Monitor", name);
        g_free(name);
    }
    ctk_config_add_timer(ctk_config, AMD_THERMAL_UPDATE_INTERVAL_MS,
                         timer_name, (GSourceFunc) update_thermal,
                         (gpointer) ctk);
    g_free(timer_name);

    gtk_widget_show_all(GTK_WIDGET(ctk));
    update_thermal(ctk);

    return GTK_WIDGET(ctk);
}


GtkTextBuffer *ctk_amd_thermal_create_help(GtkTextTagTable *table,
                                           CtkAmdThermal *ctk)
{
    GtkTextIter i;
    GtkTextBuffer *b = gtk_text_buffer_new(table);

    gtk_text_buffer_get_iter_at_offset(b, &i, 0);

    ctk_help_title(b, &i, "AMD Thermal Settings Help");
    ctk_help_para(b, &i, "This page shows the temperature sensors and fans "
                  "of an AMD Radeon GPU, read from the amdgpu driver's "
                  "hwmon interface under %s.  The readings refresh once per "
                  "second while the page is visible.", ctk->gpu->hwmon_path);

    if (ctk->sensors) {
        ctk_help_heading(b, &i, "Thermal Sensor Information");
        ctk_help_heading(b, &i, "Target");
        ctk_help_para(b, &i, "%s", __sensor_help);
        ctk_help_heading(b, &i, "Provider");
        ctk_help_para(b, &i, "%s", __provider_help);
        ctk_help_heading(b, &i, "Slowdown Temp");
        ctk_help_para(b, &i, "%s", __slowdown_help);
        ctk_help_heading(b, &i, "Shutdown Temp");
        ctk_help_para(b, &i, "%s", __shutdown_help);
        ctk_help_heading(b, &i, "Temperature");
        ctk_help_para(b, &i, "%s  %s", __reading_help, __gauge_help);
    }

    if (ctk->fan_curve_label) {
        ctk_help_heading(b, &i, "Fan Curve");
        ctk_help_para(b, &i, "%s", __fan_curve_help);
    }

    if (ctk->fans) {
        ctk_help_heading(b, &i, "Fan Information");
        ctk_help_heading(b, &i, "ID");
        ctk_help_para(b, &i, "%s", __fan_id_help);
        ctk_help_heading(b, &i, "Speed (RPM)");
        ctk_help_para(b, &i, "%s", __fan_rpm_help);
        ctk_help_heading(b, &i, "Speed (%%)");
        ctk_help_para(b, &i, "%s", __fan_speed_help);
        ctk_help_heading(b, &i, "Control Type");
        ctk_help_para(b, &i, "%s", __fan_mode_help);
        ctk_help_heading(b, &i, "Range (RPM)");
        ctk_help_para(b, &i, "%s", __fan_range_help);
    }

    if (ctk->have_fan_control) {
        ctk_help_heading(b, &i, "Enable GPU Fan Settings");
        ctk_help_para(b, &i, "%s", __enable_help);
        ctk_help_heading(b, &i, "Apply");
        ctk_help_para(b, &i, "%s", __apply_help);
        ctk_help_heading(b, &i, "Reset Hardware Defaults");
        ctk_help_para(b, &i, "%s", __reset_help);
    }

    ctk_help_finish(b);
    return b;
}


void ctk_amd_thermal_start_timer(GtkWidget *widget)
{
    CtkAmdThermal *ctk = CTK_AMD_THERMAL(widget);

    update_thermal(ctk);
    ctk_config_start_timer(ctk->ctk_config, (GSourceFunc) update_thermal,
                           (gpointer) ctk);
}


void ctk_amd_thermal_stop_timer(GtkWidget *widget)
{
    CtkAmdThermal *ctk = CTK_AMD_THERMAL(widget);

    ctk_config_stop_timer(ctk->ctk_config, (GSourceFunc) update_thermal,
                          (gpointer) ctk);
}
