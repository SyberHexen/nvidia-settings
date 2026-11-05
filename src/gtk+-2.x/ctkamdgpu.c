/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * AMD/Radeon GPU information page, laid out like the NVIDIA GPU page.  All
 * data comes from the amdgpu kernel driver's sysfs interface, so this page
 * works on any display server (X11 or Wayland) and without NV-CONTROL.
 * Clocks and power management are on the PowerMizer page and temperatures
 * and fans on the Thermal Settings page.
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
#include <sys/utsname.h>

#include <gtk/gtk.h>

#include "ctkamdgpu.h"
#include "ctkbanner.h"
#include "ctkhelp.h"
#include "ctkutils.h"

#define AMD_GPU_UPDATE_INTERVAL_MS 1000


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

static void set_label(GtkWidget *label, gchar *text /* takes ownership */)
{
    if (label) {
        gtk_label_set_text(GTK_LABEL(label), text ? text : "N/A");
    }
    g_free(text);
}


static GtkWidget *add_row(GtkWidget *table, gint *row, const gchar *name,
                          gchar *value /* owned */)
{
    GtkWidget *label;

    gtk_table_resize(GTK_TABLE(table), *row + 1, 2);
    label = add_table_row(table, *row, 0, 0.5, name, 0, 0.5,
                          value ? value : "");
    g_free(value);
    (*row)++;
    return label;
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


/* PCIe generation from a link speed: "16.0 GT/s" -> "Gen4" */
static gchar *pcie_generation(const gchar *speed)
{
    static const struct { gdouble gts; gint gen; } gens[] = {
        { 2.5, 1 }, { 5.0, 2 }, { 8.0, 3 }, { 16.0, 4 }, { 32.0, 5 },
        { 64.0, 6 },
    };
    gdouble gts;
    guint i;

    if (!speed) {
        return NULL;
    }
    gts = g_ascii_strtod(speed, NULL);
    for (i = 0; i < G_N_ELEMENTS(gens); i++) {
        if (gts > gens[i].gts - 0.1 && gts < gens[i].gts + 0.1) {
            return g_strdup_printf("Gen%d", gens[i].gen);
        }
    }
    return NULL;
}


/* "0000:10:00.0" -> "PCI:16:0:0" (the X configuration BusID format) */
static gchar *format_bus_id(const gchar *slot)
{
    guint domain, bus, dev, func;

    if (!slot || sscanf(slot, "%x:%x:%x.%x", &domain, &bus, &dev, &func) != 4) {
        return NULL;
    }
    if (domain) {
        return g_strdup_printf("PCI:%u@%u:%u:%u", bus, domain, dev, func);
    }
    return g_strdup_printf("PCI:%u:%u:%u", bus, dev, func);
}


static gchar *read_connector_edid_name(AmdConnector *c)
{
    gchar *path = g_build_filename(c->sysfs_path, "edid", NULL);
    gchar *data = NULL, *name = NULL;
    gsize len = 0;

    if (g_file_get_contents(path, &data, &len, NULL)) {
        name = amdgpu_edid_monitor_name((const guchar *) data, len);
    }
    g_free(data);
    g_free(path);
    return name;
}


/* Connected displays, one per line: "HISENSE (DP-1)" */
static gchar *make_display_device_list(AmdGpu *gpu)
{
    GString *s = g_string_new(NULL);
    GList *l;

    for (l = gpu->connectors; l; l = l->next) {
        AmdConnector *c = l->data;
        gchar *status = amdgpu_read_string(c->sysfs_path, "status");

        if (status && strcmp(status, "connected") == 0) {
            gchar *name = read_connector_edid_name(c);
            if (s->len) {
                g_string_append(s, ",\n");
            }
            if (name) {
                g_string_append_printf(s, "%s (%s)", name, c->name);
            } else {
                g_string_append(s, c->name);
            }
            g_free(name);
        }
        g_free(status);
    }
    if (!s->len) {
        g_string_append(s, "None");
    }
    return g_string_free(s, FALSE);
}


/* ---- live update -------------------------------------------------------- */

static gboolean update_gpu_usage(gpointer user_data)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(user_data);
    const gchar *dev = ctk->gpu->device_path;
    gint64 v, total;

    if (ctk->gpu_utilization) {
        set_label(ctk->gpu_utilization,
                  amdgpu_read_int64(dev, "gpu_busy_percent", &v) ?
                  g_strdup_printf("%" G_GINT64_FORMAT " %%", v) : NULL);
    }
    if (ctk->mem_utilization) {
        set_label(ctk->mem_utilization,
                  amdgpu_read_int64(dev, "mem_busy_percent", &v) ?
                  g_strdup_printf("%" G_GINT64_FORMAT " %%", v) : NULL);
    }
    if (ctk->memory_used) {
        gchar *s = NULL;
        if (amdgpu_read_int64(dev, "mem_info_vram_used", &v)) {
            if (amdgpu_read_int64(dev, "mem_info_vram_total", &total) &&
                total > 0) {
                s = g_strdup_printf("%" G_GINT64_FORMAT " MB (%.0f%%)",
                                    v >> 20, 100.0 * v / total);
            } else {
                s = g_strdup_printf("%" G_GINT64_FORMAT " MB", v >> 20);
            }
        }
        set_label(ctk->memory_used, s);
    }

    return TRUE;
}


/* ---- construction ------------------------------------------------------- */

GtkWidget *ctk_amd_gpu_new(AmdGpu *gpu, CtkConfig *ctk_config)
{
    GObject *object;
    CtkAmdGpu *ctk;
    GtkWidget *banner, *vbox, *table;
    const gchar *dev;
    struct utsname uts;
    gint row = 0;
    gint64 vram = 0, gtt = 0;
    gboolean have_vram, have_gtt;
    gchar *s, *timer_name, *name;
    gchar *max_speed, *max_width;

    g_return_val_if_fail(gpu != NULL, NULL);

    object = g_object_new(CTK_TYPE_AMD_GPU, NULL);
    ctk = CTK_AMD_GPU(object);
    ctk->gpu = gpu;
    ctk->ctk_config = ctk_config;
    dev = gpu->device_path;

    gtk_box_set_spacing(GTK_BOX(ctk), 10);

    banner = ctk_banner_image_new(BANNER_ARTWORK_GPU);
    gtk_box_pack_start(GTK_BOX(ctk), banner, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(ctk), vbox, TRUE, TRUE, 0);

    table = ctk_section_table_new(vbox, "Graphics Card Information");

    /* Processor, driver and memory */

    add_row(table, &row, "Graphics Processor:", g_strdup(gpu->product_name));
    add_row(table, &row, "Kernel Driver:",
            uname(&uts) == 0 ?
            g_strdup_printf("amdgpu (Linux %s)", uts.release) :
            g_strdup("amdgpu"));

    s = amdgpu_read_string(dev, "unique_id");
    if (s && s[0]) {
        add_row(table, &row, "GPU Unique ID:", s);
    } else {
        g_free(s);
    }

    s = amdgpu_read_string(dev, "vbios_version");
    if (s && s[0]) {
        add_row(table, &row, "VBIOS Version:", s);
    } else {
        g_free(s);
    }

    have_vram = amdgpu_read_int64(dev, "mem_info_vram_total", &vram);
    have_gtt = amdgpu_read_int64(dev, "mem_info_gtt_total", &gtt);
    if (have_vram && have_gtt) {
        add_row(table, &row, "Total Memory:",
                g_strdup_printf("%" G_GINT64_FORMAT " MB", (vram + gtt) >> 20));
    }
    if (have_vram) {
        add_row(table, &row, "Total Dedicated Memory:",
                g_strdup_printf("%" G_GINT64_FORMAT " MB", vram >> 20));
    }
    if (amdgpu_file_exists(dev, "mem_info_vram_used")) {
        ctk->memory_used = add_row(table, &row, "Used Dedicated Memory:", NULL);
    }
    if (have_gtt) {
        add_row(table, &row, "Total Shared System Memory:",
                g_strdup_printf("%" G_GINT64_FORMAT " MB", gtt >> 20));
    }

    if (amdgpu_file_exists(dev, "gpu_busy_percent")) {
        ctk->gpu_utilization = add_row(table, &row, "GPU Utilization:", NULL);
    }
    if (amdgpu_file_exists(dev, "mem_busy_percent")) {
        ctk->mem_utilization = add_row(table, &row, "Memory Utilization:",
                                       NULL);
    }

    /* Bus */

    max_speed = read_link_speed(dev, "max_link_speed");
    max_width = amdgpu_read_string(dev, "max_link_width");

    row += 3;   /* spacing, as on the NVIDIA page */
    if (max_speed || max_width) {
        gchar *gen = pcie_generation(max_speed);
        add_row(table, &row, "Bus Type:",
                g_strdup_printf("PCI Express%s%s%s%s",
                                max_width ? " x" : "",
                                max_width ? max_width : "",
                                gen ? " " : "", gen ? gen : ""));
        g_free(gen);
    } else {
        add_row(table, &row, "Bus Type:", g_strdup("PCI"));
    }
    s = format_bus_id(gpu->pci_slot);
    if (s) {
        add_row(table, &row, "Bus ID:", s);
    }
    add_row(table, &row, "PCI Device ID:",
            g_strdup_printf("0x%04x", gpu->device_id));
    add_row(table, &row, "PCI Vendor ID:",
            g_strdup_printf("0x%04x", gpu->vendor_id));
    add_row(table, &row, "PCI Subsystem ID:",
            g_strdup_printf("0x%04x:0x%04x", gpu->subsys_vendor_id,
                            gpu->subsys_device_id));
    add_row(table, &row, "PCI Revision:",
            g_strdup_printf("0x%02x", gpu->revision));
    s = amdgpu_read_string(dev, "irq");
    if (s && s[0]) {
        add_row(table, &row, "IRQ:", s);
    } else {
        g_free(s);
    }
    add_row(table, &row, "DRM Device:",
            g_strdup_printf("/dev/dri/%s", gpu->card_name));

    if (max_speed || max_width) {
        gchar *gen = pcie_generation(max_speed);

        row += 3;
        if (gen) {
            add_row(table, &row, "PCIe Generation:", gen);
        }
        if (max_width) {
            add_row(table, &row, "Maximum PCIe Link Width:",
                    g_strdup_printf("x%s", max_width));
        }
        if (max_speed) {
            add_row(table, &row, "Maximum PCIe Link Speed:",
                    g_strdup(max_speed));
        }
    }
    g_free(max_speed);
    g_free(max_width);

    /* Displays */

    row += 3;
    ctk->displays = add_row(table, &row, "Display Devices:",
                            make_display_device_list(gpu));
    gtk_misc_set_alignment(GTK_MISC(ctk->displays), 0.0f, 0.0f);

    name = drm_gpu_name(gpu->card_name);
    timer_name = g_strdup_printf("%s (AMD) Monitor", name);
    ctk_config_add_timer(ctk_config, AMD_GPU_UPDATE_INTERVAL_MS, timer_name,
                         (GSourceFunc) update_gpu_usage, (gpointer) ctk);
    g_free(timer_name);
    g_free(name);

    update_gpu_usage(ctk);
    gtk_widget_show_all(GTK_WIDGET(ctk));

    return GTK_WIDGET(ctk);
}


GtkTextBuffer *ctk_amd_gpu_create_help(GtkTextTagTable *table,
                                       CtkAmdGpu *ctk)
{
    GtkTextIter i;
    GtkTextBuffer *b = gtk_text_buffer_new(table);

    gtk_text_buffer_get_iter_at_offset(b, &i, 0);

    ctk_help_title(b, &i, "Graphics Card Information Help");
    ctk_help_para(b, &i, "This page shows information about an AMD Radeon "
                  "GPU driven by the open source amdgpu kernel driver.  All "
                  "data is read from the driver's sysfs interface under %s.  "
                  "Clocks and power management are on the PowerMizer page, "
                  "and temperatures and fans on the Thermal Settings page.",
                  ctk->gpu->device_path);

    ctk_help_heading(b, &i, "Graphics Processor");
    ctk_help_para(b, &i, "This is the product name of the GPU.");

    ctk_help_heading(b, &i, "Kernel Driver");
    ctk_help_para(b, &i, "The kernel driver that runs the GPU and the "
                  "version of the running Linux kernel.");

    ctk_help_heading(b, &i, "VBIOS Version");
    ctk_help_para(b, &i, "This is the Video BIOS version.");

    ctk_help_heading(b, &i, "Total Memory");
    ctk_help_para(b, &i, "This is the total amount of memory the GPU can "
                  "use: its dedicated video memory (VRAM) plus the system "
                  "memory the driver lets it map (GTT).");

    ctk_help_heading(b, &i, "Total Dedicated Memory");
    ctk_help_para(b, &i, "This is the amount of video memory (VRAM) "
                  "dedicated to the GPU.  On an integrated GPU this is the "
                  "system memory reserved for it by the firmware.");

    ctk_help_heading(b, &i, "Used Dedicated Memory");
    ctk_help_para(b, &i, "This is the amount of dedicated video memory in "
                  "use, and its percentage of the total.");

    ctk_help_heading(b, &i, "Total Shared System Memory");
    ctk_help_para(b, &i, "This is the amount of system memory the GPU can "
                  "map through its GART (GTT).");

    if (ctk->gpu_utilization) {
        ctk_help_heading(b, &i, "GPU Utilization");
        ctk_help_para(b, &i, "This is the percentage of time the GPU was "
                      "busy.");
    }
    if (ctk->mem_utilization) {
        ctk_help_heading(b, &i, "Memory Utilization");
        ctk_help_para(b, &i, "This is the percentage of time the memory "
                      "controller was busy.");
    }

    ctk_help_heading(b, &i, "Bus Type");
    ctk_help_para(b, &i, "This is the bus type used to connect the GPU to "
                  "the rest of the system, with its maximum link width and "
                  "PCIe generation.");

    ctk_help_heading(b, &i, "Bus ID");
    ctk_help_para(b, &i, "This is the GPU's PCI identification string, in "
                  "the format used by the BusID option of the X "
                  "configuration file.");

    ctk_help_heading(b, &i, "PCI Device ID");
    ctk_help_para(b, &i, "This is the PCI Device ID of the GPU.");

    ctk_help_heading(b, &i, "PCI Vendor ID");
    ctk_help_para(b, &i, "This is the PCI Vendor ID of the GPU.");

    ctk_help_heading(b, &i, "IRQ");
    ctk_help_para(b, &i, "This is the interrupt request line assigned to "
                  "this GPU.");

    ctk_help_heading(b, &i, "DRM Device");
    ctk_help_para(b, &i, "This is the kernel DRM device node of the GPU.");

    ctk_help_heading(b, &i, "PCIe Generation");
    ctk_help_para(b, &i, "This is the PCIe generation of the GPU's link.");

    ctk_help_heading(b, &i, "Maximum PCIe Link Width");
    ctk_help_para(b, &i, "This is the maximum width that the PCIe link "
                  "between the GPU and the system may be trained to.  This "
                  "is expressed in number of lanes.");

    ctk_help_heading(b, &i, "Maximum PCIe Link Speed");
    ctk_help_para(b, &i, "This is the maximum speed that the PCIe link "
                  "between the GPU and the system may be trained to, in "
                  "gigatransfers per second (GT/s).");

    ctk_help_heading(b, &i, "Display Devices");
    ctk_help_para(b, &i, "This is the list of display devices connected to "
                  "the GPU, with the connector each one is on.");

    ctk_help_finish(b);
    return b;
}


void ctk_amd_gpu_start_timer(GtkWidget *widget)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(widget);

    update_gpu_usage(ctk);
    ctk_config_start_timer(ctk->ctk_config, (GSourceFunc) update_gpu_usage,
                           (gpointer) ctk);
}


void ctk_amd_gpu_stop_timer(GtkWidget *widget)
{
    CtkAmdGpu *ctk = CTK_AMD_GPU(widget);

    ctk_config_stop_timer(ctk->ctk_config, (GSourceFunc) update_gpu_usage,
                          (gpointer) ctk);
}
