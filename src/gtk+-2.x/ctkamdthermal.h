/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon GPU thermal page: temperature sensors and fans.
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

#ifndef __CTK_AMD_THERMAL_H__
#define __CTK_AMD_THERMAL_H__

#include <gtk/gtk.h>

#include "ctkconfig.h"
#include "amdgpu-sysfs.h"

G_BEGIN_DECLS

#define CTK_TYPE_AMD_THERMAL (ctk_amd_thermal_get_type())

#define CTK_AMD_THERMAL(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), CTK_TYPE_AMD_THERMAL, CtkAmdThermal))

#define CTK_AMD_THERMAL_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST ((klass), CTK_TYPE_AMD_THERMAL, \
                              CtkAmdThermalClass))

#define CTK_IS_AMD_THERMAL(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CTK_TYPE_AMD_THERMAL))

typedef struct _CtkAmdThermal       CtkAmdThermal;
typedef struct _CtkAmdThermalClass  CtkAmdThermalClass;

struct _CtkAmdThermal
{
    GtkVBox parent;

    CtkConfig *ctk_config;
    AmdGpu *gpu;                 /* not owned: shared with the GPU page */

    GList *sensors;              /* CtkAmdSensor* */
    GList *fans;                 /* CtkAmdFan* */

    /* fan control (pwm1_enable / pwm1) */
    GtkWidget *enable_checkbox;
    GtkWidget *apply_button;
    GtkWidget *reset_button;
    gboolean have_fan_control;
    gboolean fan_values_changed;

    /* RDNA3+ fan curve (gpu_od/fan_ctrl/fan_curve), read only */
    GtkWidget *fan_curve_label;

    gboolean updating;           /* suppress signal handlers */
};

struct _CtkAmdThermalClass
{
    GtkVBoxClass parent_class;
};

GType          ctk_amd_thermal_get_type    (void) G_GNUC_CONST;
GtkWidget*     ctk_amd_thermal_new         (AmdGpu *gpu, CtkConfig *ctk_config);
GtkTextBuffer* ctk_amd_thermal_create_help (GtkTextTagTable *,
                                            CtkAmdThermal *);

void           ctk_amd_thermal_start_timer (GtkWidget *);
void           ctk_amd_thermal_stop_timer  (GtkWidget *);

G_END_DECLS

#endif /* __CTK_AMD_THERMAL_H__ */
