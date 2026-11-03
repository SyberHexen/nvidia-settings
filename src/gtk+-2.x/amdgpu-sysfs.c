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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

#include <glib.h>

#include "amdgpu-sysfs.h"

#define DRM_SYSFS_DIR "/sys/class/drm"

static const gchar *pci_ids_paths[] = {
    "/usr/share/hwdata/pci.ids",
    "/usr/share/misc/pci.ids",
    "/usr/share/pci.ids",
    "/usr/local/share/pci.ids",
    NULL
};


static gchar *build_path(const gchar *dir, const gchar *file)
{
    if (!dir || file[0] == '/') {
        return g_strdup(file);
    }
    return g_build_filename(dir, file, NULL);
}


gchar *amdgpu_read_string(const gchar *dir, const gchar *file)
{
    gchar *path = build_path(dir, file);
    gchar *contents = NULL;

    if (!g_file_get_contents(path, &contents, NULL, NULL)) {
        contents = NULL;
    } else {
        g_strchomp(contents);
    }
    g_free(path);
    return contents;
}


gboolean amdgpu_read_int64(const gchar *dir, const gchar *file, gint64 *out)
{
    gchar *str = amdgpu_read_string(dir, file);
    gchar *end = NULL;
    gint64 val;

    if (!str) {
        return FALSE;
    }

    errno = 0;
    val = g_ascii_strtoll(g_strstrip(str), &end, 0);
    if (errno != 0 || end == str) {
        g_free(str);
        return FALSE;
    }
    g_free(str);
    *out = val;
    return TRUE;
}


gboolean amdgpu_file_exists(const gchar *dir, const gchar *file)
{
    gchar *path = build_path(dir, file);
    gboolean ret = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    return ret;
}


gboolean amdgpu_file_writable(const gchar *dir, const gchar *file)
{
    gchar *path = build_path(dir, file);
    gboolean ret = (access(path, W_OK) == 0);
    g_free(path);
    return ret;
}


/*
 * Write directly; if that fails with a permission error, retry through
 * pkexec.  The value and path are passed as positional parameters to the
 * shell so they are never interpreted as shell syntax.
 */

gboolean amdgpu_write_string(const gchar *dir, const gchar *file,
                             const gchar *value, gchar **error_msg)
{
    gchar *path = build_path(dir, file);
    gboolean ret = FALSE;
    FILE *fp;
    int err = 0;

    if (error_msg) {
        *error_msg = NULL;
    }

    /* Only ever touch sysfs files. */
    if (!g_str_has_prefix(path, "/sys/")) {
        if (error_msg) {
            *error_msg = g_strdup_printf("Refusing to write outside sysfs: %s",
                                         path);
        }
        goto done;
    }

    fp = fopen(path, "w");
    if (fp) {
        if (fputs(value, fp) >= 0 && fflush(fp) == 0) {
            ret = TRUE;
        } else {
            err = errno;
        }
        if (fclose(fp) != 0 && ret) {
            err = errno;
            ret = FALSE;
        }
        if (ret || (err != EACCES && err != EPERM)) {
            if (!ret && error_msg) {
                *error_msg = g_strdup_printf("Unable to write '%s' to %s: %s",
                                             value, path, g_strerror(err));
            }
            goto done;
        }
    } else {
        err = errno;
        if (err != EACCES && err != EPERM) {
            if (error_msg) {
                *error_msg = g_strdup_printf("Unable to open %s: %s",
                                             path, g_strerror(err));
            }
            goto done;
        }
    }

    /* Permission denied: escalate via polkit. */
    {
        gchar *argv[] = {
            "pkexec", "/bin/sh", "-c", "printf '%s' \"$1\" > \"$2\"",
            "sh", (gchar *) value, path, NULL
        };
        gint status = 0;
        gchar *std_err = NULL;
        GError *gerr = NULL;

        if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                          NULL, NULL, NULL, &std_err, &status, &gerr)) {
            if (error_msg) {
                *error_msg = g_strdup_printf(
                    "Root privileges are required to write %s, and pkexec "
                    "could not be run (%s).  Run nvidia-settings as root or "
                    "install polkit.", path, gerr ? gerr->message : "unknown");
            }
            g_clear_error(&gerr);
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            ret = TRUE;
        } else if (error_msg) {
            g_strstrip(std_err ? std_err : (std_err = g_strdup("")));
            *error_msg = g_strdup_printf(
                "Failed to write '%s' to %s with elevated privileges%s%s",
                value, path, std_err[0] ? ": " : ".", std_err);
        }
        g_free(std_err);
    }

 done:
    g_free(path);
    return ret;
}


/* ---- PCI name lookup ---------------------------------------------------- */

static gchar *lookup_pci_name(guint vendor, guint device)
{
    gint i;

    for (i = 0; pci_ids_paths[i]; i++) {
        FILE *fp = fopen(pci_ids_paths[i], "r");
        char line[512];
        gboolean in_vendor = FALSE;

        if (!fp) {
            continue;
        }

        while (fgets(line, sizeof(line), fp)) {
            unsigned int id;

            if (line[0] == '#' || line[0] == '\n') {
                continue;
            }
            if (line[0] != '\t') {
                if (in_vendor) {
                    break;   /* past our vendor's block */
                }
                if (sscanf(line, "%4x", &id) == 1 && id == vendor) {
                    in_vendor = TRUE;
                }
                continue;
            }
            if (!in_vendor || line[1] == '\t') {
                continue;
            }
            if (sscanf(line + 1, "%4x", &id) == 1 && id == device) {
                gchar *name = g_strstrip(g_strdup(line + 1 + 4));
                fclose(fp);
                return name;
            }
        }
        fclose(fp);
    }
    return NULL;
}


/*
 * pci.ids names look like "Raphael [Radeon 610M]".  Turn that into
 * "AMD Radeon 610M (Raphael)" which reads better as a page title.
 */
static gchar *make_product_name(guint vendor, guint device)
{
    gchar *raw = lookup_pci_name(vendor, device);
    gchar *ret;
    gchar *lb, *rb;

    if (!raw) {
        return g_strdup_printf("AMD Radeon GPU [%04x:%04x]", vendor, device);
    }

    lb = strchr(raw, '[');
    rb = lb ? strrchr(lb, ']') : NULL;
    if (lb && rb && rb > lb + 1) {
        gchar *codename = g_strstrip(g_strndup(raw, lb - raw));
        gchar *market = g_strndup(lb + 1, rb - lb - 1);
        const gchar *prefix =
            g_str_has_prefix(market, "AMD") ? "" : "AMD ";

        if (codename[0]) {
            ret = g_strdup_printf("%s%s (%s)", prefix, market, codename);
        } else {
            ret = g_strdup_printf("%s%s", prefix, market);
        }
        g_free(codename);
        g_free(market);
    } else {
        ret = g_strdup_printf("AMD %s", raw);
    }
    g_free(raw);
    return ret;
}


/* ---- Enumeration -------------------------------------------------------- */

static gboolean is_card_dir(const gchar *name)
{
    const gchar *p;

    if (!g_str_has_prefix(name, "card")) {
        return FALSE;
    }
    for (p = name + 4; *p; p++) {
        if (!isdigit((unsigned char) *p)) {
            return FALSE;
        }
    }
    return p != name + 4;
}


static gchar *find_hwmon(const gchar *device_path)
{
    gchar *hwmon_dir = g_build_filename(device_path, "hwmon", NULL);
    GDir *dir = g_dir_open(hwmon_dir, 0, NULL);
    const gchar *entry;
    gchar *ret = NULL;

    if (dir) {
        while ((entry = g_dir_read_name(dir))) {
            if (g_str_has_prefix(entry, "hwmon")) {
                ret = g_build_filename(hwmon_dir, entry, NULL);
                break;
            }
        }
        g_dir_close(dir);
    }
    g_free(hwmon_dir);
    return ret;
}


static gint compare_connectors(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(((const AmdConnector *) a)->name,
                     ((const AmdConnector *) b)->name);
}


static GList *find_connectors(const gchar *card_name)
{
    GDir *dir = g_dir_open(DRM_SYSFS_DIR, 0, NULL);
    const gchar *entry;
    gchar *prefix = g_strdup_printf("%s-", card_name);
    GList *list = NULL;

    if (!dir) {
        g_free(prefix);
        return NULL;
    }

    while ((entry = g_dir_read_name(dir))) {
        AmdConnector *c;
        gint64 id;

        if (!g_str_has_prefix(entry, prefix)) {
            continue;
        }
        c = g_new0(AmdConnector, 1);
        c->name = g_strdup(entry + strlen(prefix));
        c->sysfs_path = g_build_filename(DRM_SYSFS_DIR, entry, NULL);
        c->connector_id =
            amdgpu_read_int64(c->sysfs_path, "connector_id", &id) ? id : -1;
        list = g_list_prepend(list, c);
    }
    g_dir_close(dir);
    g_free(prefix);

    return g_list_sort(list, compare_connectors);
}


static gint compare_gpus(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(((const AmdGpu *) a)->pci_slot,
                     ((const AmdGpu *) b)->pci_slot);
}


GList *amdgpu_enumerate(void)
{
    GDir *dir = g_dir_open(DRM_SYSFS_DIR, 0, NULL);
    const gchar *entry;
    GList *list = NULL, *l;
    gint idx = 0;

    if (!dir) {
        return NULL;
    }

    while ((entry = g_dir_read_name(dir))) {
        gchar *device_path, *driver_link, *driver;
        gint64 v;
        AmdGpu *gpu;

        if (!is_card_dir(entry)) {
            continue;
        }

        device_path = g_build_filename(DRM_SYSFS_DIR, entry, "device", NULL);
        driver_link = g_build_filename(device_path, "driver", NULL);
        driver = g_file_read_link(driver_link, NULL);
        g_free(driver_link);

        if (!driver) {
            g_free(device_path);
            continue;
        }
        {
            gchar *base = g_path_get_basename(driver);
            gboolean is_amdgpu = (g_strcmp0(base, "amdgpu") == 0);
            g_free(base);
            g_free(driver);
            if (!is_amdgpu) {
                g_free(device_path);
                continue;
            }
        }

        gpu = g_new0(AmdGpu, 1);
        gpu->card_name = g_strdup(entry);
        gpu->card_path = g_build_filename(DRM_SYSFS_DIR, entry, NULL);
        gpu->device_path = device_path;
        gpu->hwmon_path = find_hwmon(device_path);

        gpu->vendor_id = amdgpu_read_int64(device_path, "vendor", &v) ? v : 0;
        gpu->device_id = amdgpu_read_int64(device_path, "device", &v) ? v : 0;
        gpu->subsys_vendor_id =
            amdgpu_read_int64(device_path, "subsystem_vendor", &v) ? v : 0;
        gpu->subsys_device_id =
            amdgpu_read_int64(device_path, "subsystem_device", &v) ? v : 0;
        gpu->revision = amdgpu_read_int64(device_path, "revision", &v) ? v : 0;

        {
            gchar *link = g_file_read_link(device_path, NULL);
            gpu->pci_slot = link ? g_path_get_basename(link)
                                 : g_strdup("unknown");
            g_free(link);
        }

        gpu->product_name = make_product_name(gpu->vendor_id, gpu->device_id);
        gpu->connectors = find_connectors(entry);

        list = g_list_prepend(list, gpu);
    }
    g_dir_close(dir);

    list = g_list_sort(list, compare_gpus);
    for (l = list; l; l = l->next) {
        ((AmdGpu *) l->data)->index = idx++;
    }
    return list;
}


static void free_connector(gpointer data)
{
    AmdConnector *c = data;
    g_free(c->name);
    g_free(c->sysfs_path);
    g_free(c);
}


void amdgpu_free(AmdGpu *gpu)
{
    if (!gpu) {
        return;
    }
    g_free(gpu->card_name);
    g_free(gpu->card_path);
    g_free(gpu->device_path);
    g_free(gpu->hwmon_path);
    g_free(gpu->pci_slot);
    g_free(gpu->product_name);
    g_list_free_full(gpu->connectors, free_connector);
    g_free(gpu);
}


void amdgpu_free_list(GList *gpus)
{
    g_list_free_full(gpus, (GDestroyNotify) amdgpu_free);
}


/* ---- DPM tables --------------------------------------------------------- */

/*
 * Format:
 *   0: 400Mhz
 *   1: 700Mhz
 *   2: 2200Mhz *
 */
GList *amdgpu_read_dpm_levels(const gchar *device_path, const gchar *file)
{
    gchar *contents = amdgpu_read_string(device_path, file);
    gchar **lines;
    GList *list = NULL;
    gint i;

    if (!contents) {
        return NULL;
    }

    lines = g_strsplit(contents, "\n", -1);
    for (i = 0; lines[i]; i++) {
        gchar *colon = strchr(lines[i], ':');
        gchar *end = NULL;
        glong idx;
        AmdDpmLevel *lvl;

        if (!colon) {
            continue;
        }
        idx = strtol(lines[i], &end, 10);
        if (end == lines[i] || end != colon) {
            continue;
        }

        lvl = g_new0(AmdDpmLevel, 1);
        lvl->index = idx;
        lvl->active = (strchr(colon, '*') != NULL);
        lvl->label = g_strstrip(g_strdup(colon + 1));
        {
            gchar *star = strchr(lvl->label, '*');
            if (star) {
                *star = '\0';
                g_strstrip(lvl->label);
            }
        }
        list = g_list_append(list, lvl);
    }
    g_strfreev(lines);
    g_free(contents);
    return list;
}


static void free_dpm_level(gpointer data)
{
    AmdDpmLevel *l = data;
    g_free(l->label);
    g_free(l);
}


void amdgpu_free_dpm_levels(GList *levels)
{
    g_list_free_full(levels, free_dpm_level);
}


/* Returns the active level's label; if several are active (common for
 * mclk on APUs), they are joined with " / ". */
gchar *amdgpu_dpm_current_label(const gchar *device_path, const gchar *file)
{
    GList *levels = amdgpu_read_dpm_levels(device_path, file), *l;
    GString *s = g_string_new(NULL);

    for (l = levels; l; l = l->next) {
        AmdDpmLevel *lvl = l->data;
        if (lvl->active && !strstr(s->str, lvl->label)) {
            if (s->len) {
                g_string_append(s, " / ");
            }
            g_string_append(s, lvl->label);
        }
    }
    amdgpu_free_dpm_levels(levels);

    if (s->len == 0) {
        g_string_free(s, TRUE);
        return NULL;
    }
    return g_string_free(s, FALSE);
}


/* ---- Power profiles ----------------------------------------------------- */

/*
 * Older GPUs:   "  0 BOOTUP_DEFAULT :"  / " 1 3D_FULL_SCREEN*:"
 * Newer GPUs print a table where profile header lines look like
 *               "  1 3D_FULL_SCREEN*:"  followed by indented
 *               "        0(       GFXCLK) ..." detail rows,
 * and a column header "PROFILE_INDEX(NAME) ..." line.
 */
GList *amdgpu_read_power_profiles(const gchar *device_path)
{
    gchar *contents = amdgpu_read_string(device_path, "pp_power_profile_mode");
    gchar **lines;
    GList *list = NULL;
    gint i;

    if (!contents) {
        return NULL;
    }

    lines = g_strsplit(contents, "\n", -1);
    for (i = 0; lines[i]; i++) {
        gchar *p = lines[i];
        gchar *end;
        glong idx;
        GString *name;
        AmdPowerProfile *prof;

        while (*p == ' ' || *p == '\t') p++;
        if (!isdigit((unsigned char) *p)) {
            continue;
        }
        idx = strtol(p, &end, 10);
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (!(isupper((unsigned char) *p) || isdigit((unsigned char) *p))) {
            continue;   /* detail row such as "0(  GFXCLK)" */
        }

        name = g_string_new(NULL);
        while (*p && (isalnum((unsigned char) *p) || *p == '_')) {
            g_string_append_c(name, *p++);
        }

        prof = g_new0(AmdPowerProfile, 1);
        prof->index = idx;
        prof->active = (strchr(p, '*') != NULL);
        prof->name = g_string_free(name, FALSE);
        list = g_list_append(list, prof);
    }
    g_strfreev(lines);
    g_free(contents);
    return list;
}


static void free_power_profile(gpointer data)
{
    AmdPowerProfile *p = data;
    g_free(p->name);
    g_free(p);
}


void amdgpu_free_power_profiles(GList *profiles)
{
    g_list_free_full(profiles, free_power_profile);
}


/* ---- EDID --------------------------------------------------------------- */

gchar *drm_edid_descriptor_string(const guchar *edid, gsize len, guchar tag)
{
    gsize off;

    if (!edid || len < 128) {
        return NULL;
    }

    /* Four 18-byte descriptors in the base block */
    for (off = 54; off + 18 <= 126; off += 18) {
        const guchar *d = edid + off;
        if (d[0] == 0 && d[1] == 0 && d[3] == tag) {
            gchar buf[14];
            gint i;
            for (i = 0; i < 13; i++) {
                guchar ch = d[5 + i];
                if (ch == 0x0A || ch == 0x00) break;
                buf[i] = g_ascii_isprint(ch) ? ch : '?';
            }
            buf[i] = '\0';
            g_strstrip(buf);
            return buf[0] ? g_strdup(buf) : NULL;
        }
    }
    return NULL;
}


gchar *amdgpu_edid_monitor_name(const guchar *edid, gsize len)
{
    gchar *name;

    if (!edid || len < 128) {
        return NULL;
    }
    name = drm_edid_descriptor_string(edid, len, 0xFC);
    if (name) {
        return name;
    }

    /* Fall back to the PNP manufacturer id + product code */
    {
        guint16 m = (edid[8] << 8) | edid[9];
        guint16 code = edid[10] | (edid[11] << 8);
        return g_strdup_printf("%c%c%c %04X",
                               '@' + ((m >> 10) & 0x1f),
                               '@' + ((m >> 5) & 0x1f),
                               '@' + (m & 0x1f), code);
    }
}
