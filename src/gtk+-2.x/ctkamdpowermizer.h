/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon GPU PowerMizer page: clocks, DPM performance levels and
 * power management settings, laid out like the NVIDIA PowerMizer page.
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

#ifndef __CTK_AMD_POWERMIZER_H__
#define __CTK_AMD_POWERMIZER_H__

#include <gtk/gtk.h>

#include "ctkconfig.h"
#include "amdgpu-sysfs.h"

G_BEGIN_DECLS

#define CTK_TYPE_AMD_POWERMIZER (ctk_amd_powermizer_get_type())

#define CTK_AMD_POWERMIZER(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), CTK_TYPE_AMD_POWERMIZER, \
                                 CtkAmdPowermizer))

#define CTK_AMD_POWERMIZER_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST ((klass), CTK_TYPE_AMD_POWERMIZER, \
                              CtkAmdPowermizerClass))

#define CTK_IS_AMD_POWERMIZER(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CTK_TYPE_AMD_POWERMIZER))

typedef struct _CtkAmdPowermizer       CtkAmdPowermizer;
typedef struct _CtkAmdPowermizerClass  CtkAmdPowermizerClass;

struct _CtkAmdPowermizer
{
    GtkVBox parent;

    CtkConfig *ctk_config;
    AmdGpu *gpu;                 /* not owned: shared with the GPU page */

    /* PowerMizer Information */
    GtkWidget *gpu_clock;
    GtkWidget *mem_clock;
    GtkWidget *power_draw;
    GtkWidget *link_width;
    GtkWidget *link_speed;
    GtkWidget *perf_level;

    /* Performance Levels: one column per DPM clock table */
    GList *columns;              /* const gchar* sysfs file names */
    GtkWidget *levels_hbox;
    gchar *levels_snapshot;      /* table contents last drawn */

    /* PowerMizer Settings */
    GtkWidget *mode_combo;       /* power_dpm_force_performance_level */
    GtkWidget *mode_txt;
    GtkWidget *profile_combo;    /* pp_power_profile_mode */
    GList *profiles;             /* AmdPowerProfile* */
    GtkWidget *power_cap_spin;   /* hwmon power1_cap */
    GtkWidget *sclk_combo;
    GtkWidget *mclk_combo;
    GtkWidget *manual_box;

    gboolean updating;           /* suppress signal handlers */
};

struct _CtkAmdPowermizerClass
{
    GtkVBoxClass parent_class;
};

GType          ctk_amd_powermizer_get_type    (void) G_GNUC_CONST;
GtkWidget*     ctk_amd_powermizer_new         (AmdGpu *gpu,
                                               CtkConfig *ctk_config);
GtkTextBuffer* ctk_amd_powermizer_create_help (GtkTextTagTable *,
                                               CtkAmdPowermizer *);

void           ctk_amd_powermizer_start_timer (GtkWidget *);
void           ctk_amd_powermizer_stop_timer  (GtkWidget *);

G_END_DECLS

#endif /* __CTK_AMD_POWERMIZER_H__ */
