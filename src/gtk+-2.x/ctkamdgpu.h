/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon GPU information and power management page.
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

#ifndef __CTK_AMD_GPU_H__
#define __CTK_AMD_GPU_H__

#include <gtk/gtk.h>

#include "ctkconfig.h"
#include "amdgpu-sysfs.h"

G_BEGIN_DECLS

#define CTK_TYPE_AMD_GPU (ctk_amd_gpu_get_type())

#define CTK_AMD_GPU(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), CTK_TYPE_AMD_GPU, CtkAmdGpu))

#define CTK_AMD_GPU_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST ((klass), CTK_TYPE_AMD_GPU, CtkAmdGpuClass))

#define CTK_IS_AMD_GPU(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CTK_TYPE_AMD_GPU))

typedef struct _CtkAmdGpu       CtkAmdGpu;
typedef struct _CtkAmdGpuClass  CtkAmdGpuClass;

struct _CtkAmdGpu
{
    GtkVBox parent;

    CtkConfig *ctk_config;
    AmdGpu *gpu;                 /* owned */

    /* live status labels */
    GtkWidget *gpu_busy;
    GtkWidget *mem_busy;
    GtkWidget *vram_usage;
    GtkWidget *gtt_usage;
    GtkWidget *sclk;
    GtkWidget *mclk;
    GtkWidget *power;
    GtkWidget *voltage;
    GtkWidget *fan;
    GtkWidget *perf_level_current;
    GList *temp_rows;            /* list of CtkAmdTempRow* */

    /* power management controls */
    GtkWidget *perf_level_combo;
    GtkWidget *profile_combo;
    GList *profiles;             /* AmdPowerProfile* */
    GtkWidget *power_cap_spin;
    GtkWidget *sclk_combo;
    GtkWidget *mclk_combo;
    GtkWidget *manual_box;

    gboolean updating;           /* suppress signal handlers */
};

struct _CtkAmdGpuClass
{
    GtkVBoxClass parent_class;
};

GType          ctk_amd_gpu_get_type    (void) G_GNUC_CONST;
GtkWidget*     ctk_amd_gpu_new         (AmdGpu *gpu, CtkConfig *ctk_config);
GtkTextBuffer* ctk_amd_gpu_create_help (GtkTextTagTable *, CtkAmdGpu *);

void           ctk_amd_gpu_start_timer (GtkWidget *);
void           ctk_amd_gpu_stop_timer  (GtkWidget *);

G_END_DECLS

#endif /* __CTK_AMD_GPU_H__ */
