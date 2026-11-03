/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon (amdgpu kernel driver) support: sysfs access helpers.
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

#ifndef __AMDGPU_SYSFS_H__
#define __AMDGPU_SYSFS_H__

#include <glib.h>

G_BEGIN_DECLS

#define AMDGPU_PCI_VENDOR_ID 0x1002

/* A DRM connector exposed by an amdgpu card (e.g. card1-DP-1) */
typedef struct _AmdConnector {
    gchar *name;          /* connector name without card prefix: "DP-1" */
    gchar *sysfs_path;    /* /sys/class/drm/card1-DP-1 */
    gint   connector_id;  /* DRM connector object id, or -1 if unknown */
} AmdConnector;

typedef struct _AmdGpu {
    gint   index;         /* index among detected AMD GPUs */
    gchar *card_name;     /* "card1" */
    gchar *card_path;     /* /sys/class/drm/card1 */
    gchar *device_path;   /* /sys/class/drm/card1/device */
    gchar *hwmon_path;    /* /sys/class/drm/card1/device/hwmon/hwmonN or NULL */
    gchar *pci_slot;      /* "0000:10:00.0" */
    guint  vendor_id;
    guint  device_id;
    guint  subsys_vendor_id;
    guint  subsys_device_id;
    guint  revision;
    gchar *product_name;  /* human readable name, never NULL */

    GList *connectors;    /* list of AmdConnector* */
} AmdGpu;

/*
 * Returns the DRM card node name ("card0") of the PCI device at 'pci_slot'
 * ("0000:01:00.0"), or NULL if it has none.  Works for any GPU vendor.
 */
gchar   *drm_card_for_pci_slot(const gchar *pci_slot);

/* User-visible GPU name for a DRM card node: "card1" -> "GPU_1" */
gchar   *drm_gpu_name(const gchar *card);

/* Discovery */
GList   *amdgpu_enumerate(void);      /* returns list of AmdGpu* */
void     amdgpu_free_list(GList *gpus);
void     amdgpu_free(AmdGpu *gpu);

/* Generic sysfs helpers (path relative to device_path unless absolute) */
gchar   *amdgpu_read_string(const gchar *dir, const gchar *file);
gboolean amdgpu_read_int64(const gchar *dir, const gchar *file, gint64 *out);
gboolean amdgpu_file_exists(const gchar *dir, const gchar *file);
gboolean amdgpu_file_writable(const gchar *dir, const gchar *file);

/*
 * Write a value to a sysfs file.  If the file is not writable by the current
 * user, the write is retried through pkexec(1) so the user can authenticate.
 * On failure, *error_msg (if non-NULL) is set to a newly allocated string.
 */
gboolean amdgpu_write_string(const gchar *dir, const gchar *file,
                             const gchar *value, gchar **error_msg);

/* DPM clock table (pp_dpm_sclk, pp_dpm_mclk, ...) */
typedef struct _AmdDpmLevel {
    gint   index;
    gchar *label;         /* e.g. "2200Mhz" */
    gboolean active;
} AmdDpmLevel;

GList   *amdgpu_read_dpm_levels(const gchar *device_path, const gchar *file);
void     amdgpu_free_dpm_levels(GList *levels);
gchar   *amdgpu_dpm_current_label(const gchar *device_path, const gchar *file);

/* Power profile modes (pp_power_profile_mode) */
typedef struct _AmdPowerProfile {
    gint   index;
    gchar *name;
    gboolean active;
} AmdPowerProfile;

GList   *amdgpu_read_power_profiles(const gchar *device_path);
void     amdgpu_free_power_profiles(GList *profiles);

/* EDID helpers */
/* The text of the first base-block descriptor with 'tag' (0xFC monitor name,
 * 0xFF serial number, 0xFE text), or NULL. */
gchar   *drm_edid_descriptor_string(const guchar *edid, gsize len,
                                    guchar tag);
gchar   *amdgpu_edid_monitor_name(const guchar *edid, gsize len);

G_END_DECLS

#endif /* __AMDGPU_SYSFS_H__ */
