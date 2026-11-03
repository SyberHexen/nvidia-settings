/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * Extra System Information sections: Vulkan devices and AMD driver
 * information.  Vulkan is loaded at runtime with dlopen(), so neither the
 * Vulkan loader nor its headers are required to build or run
 * nvidia-settings.
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
#include <dlfcn.h>
#include <sys/utsname.h>

#include <gtk/gtk.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#if defined(__has_include)
# if __has_include(<vulkan/vulkan.h>)
#  define CTK_HAVE_VULKAN_HEADERS 1
# endif
#endif

#ifdef CTK_HAVE_VULKAN_HEADERS
# define VK_NO_PROTOTYPES
# include <vulkan/vulkan.h>
#endif

#include "ctksysinfo.h"
#include "ctkutils.h"
#include "ctkhelp.h"
#include "amdgpu-sysfs.h"
#include "ctkamddisplay.h"


/* ---- layout ------------------------------------------------------------- */

static void row(GtkWidget *table, gint *r, const gchar *name,
                const gchar *value)
{
    gtk_table_resize(GTK_TABLE(table), *r + 1, 2);
    add_table_row(table, (*r)++, 0, 0, name, 0, 0, value ? value : "Unknown");
}


/* ---- Vulkan ------------------------------------------------------------- */

typedef struct {
    gchar *gpu;          /* "GPU_0", or NULL when not a PCI device */
    gchar *name;         /* deviceName */
    gchar *api;          /* "1.4.312" */
    gchar *driver;       /* "NVIDIA 615.78.08", "radv Mesa 25.2.4" */
    gchar *driver_info;  /* raw driverInfo, may be NULL */
    gboolean radv;
    const gchar *type;
} VkDev;


static void vkdev_free(gpointer data)
{
    VkDev *d = data;
    g_free(d->gpu);
    g_free(d->name);
    g_free(d->api);
    g_free(d->driver);
    g_free(d->driver_info);
    g_free(d);
}


#ifdef CTK_HAVE_VULKAN_HEADERS

static gchar *version_string(uint32_t v)
{
    return g_strdup_printf("%u.%u.%u", VK_API_VERSION_MAJOR(v),
                           VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}


static const gchar *device_type(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU (software)";
    default:                                     return "Other";
    }
}


static gboolean has_extension(VkExtensionProperties *exts, uint32_t n,
                              const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(exts[i].extensionName, name) == 0) return TRUE;
    }
    return FALSE;
}


/*
 * Enumerate the Vulkan physical devices.  Returns FALSE if no Vulkan loader
 * is installed; otherwise *loader_version is set and *devices holds VkDev*.
 */
static gboolean probe_vulkan(gchar **loader_version, GList **devices,
                             gchar **error)
{
    void *h;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkEnumerateInstanceVersion enum_version;
    PFN_vkCreateInstance create_instance;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkEnumeratePhysicalDevices enum_devices;
    PFN_vkGetPhysicalDeviceProperties get_props;
    PFN_vkGetPhysicalDeviceProperties2 get_props2 = NULL;
    PFN_vkEnumerateDeviceExtensionProperties enum_exts;
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    VkInstance instance;
    VkPhysicalDevice *pds;
    uint32_t version = VK_API_VERSION_1_0, n = 0, i;
    VkResult res;

    *loader_version = NULL;
    *devices = NULL;
    *error = NULL;

    h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        return FALSE;
    }
    gipa = (PFN_vkGetInstanceProcAddr) dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) {
        dlclose(h);
        return FALSE;
    }

    enum_version = (PFN_vkEnumerateInstanceVersion)
        gipa(NULL, "vkEnumerateInstanceVersion");
    if (enum_version) {
        enum_version(&version);
    }
    *loader_version = version_string(version);

    create_instance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    if (!create_instance) {
        *error = g_strdup("The Vulkan loader is unusable.");
        dlclose(h);
        return TRUE;
    }

    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "nvidia-settings";
    app.apiVersion = version;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    res = create_instance(&ici, NULL, &instance);
    if (res != VK_SUCCESS) {
        *error = g_strdup_printf("Unable to create a Vulkan instance "
                                 "(VkResult %d); no Vulkan driver may be "
                                 "installed.", (int) res);
        dlclose(h);
        return TRUE;
    }

    destroy_instance = (PFN_vkDestroyInstance)
        gipa(instance, "vkDestroyInstance");
    enum_devices = (PFN_vkEnumeratePhysicalDevices)
        gipa(instance, "vkEnumeratePhysicalDevices");
    get_props = (PFN_vkGetPhysicalDeviceProperties)
        gipa(instance, "vkGetPhysicalDeviceProperties");
    enum_exts = (PFN_vkEnumerateDeviceExtensionProperties)
        gipa(instance, "vkEnumerateDeviceExtensionProperties");
    if (version >= VK_API_VERSION_1_1) {
        get_props2 = (PFN_vkGetPhysicalDeviceProperties2)
            gipa(instance, "vkGetPhysicalDeviceProperties2");
    }

    if (!enum_devices || !get_props || !enum_exts ||
        enum_devices(instance, &n, NULL) != VK_SUCCESS || n == 0) {
        *error = g_strdup("No Vulkan devices were found.");
        if (destroy_instance) destroy_instance(instance, NULL);
        dlclose(h);
        return TRUE;
    }

    pds = g_new0(VkPhysicalDevice, n);
    res = enum_devices(instance, &n, pds);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
        n = 0;
    }

    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        VkExtensionProperties *exts = NULL;
        uint32_t next = 0;
        gboolean have_driver, have_pci;
        VkDev *d = g_new0(VkDev, 1);

        get_props(pds[i], &props);
        if (enum_exts(pds[i], NULL, &next, NULL) == VK_SUCCESS && next) {
            exts = g_new0(VkExtensionProperties, next);
            if (enum_exts(pds[i], NULL, &next, exts) != VK_SUCCESS) {
                next = 0;
            }
        }

        d->name = g_strdup(props.deviceName);
        d->api = version_string(props.apiVersion);
        d->type = device_type(props.deviceType);

        have_driver = props.apiVersion >= VK_API_VERSION_1_2 ||
                      has_extension(exts, next, "VK_KHR_driver_properties");
        have_pci = has_extension(exts, next, "VK_EXT_pci_bus_info");

        if (get_props2 && props.apiVersion >= VK_API_VERSION_1_1 &&
            (have_driver || have_pci)) {
            VkPhysicalDeviceProperties2 p2;
            VkPhysicalDeviceDriverProperties drv;
            VkPhysicalDevicePCIBusInfoPropertiesEXT pci;
            void **tail;

            memset(&p2, 0, sizeof(p2));
            memset(&drv, 0, sizeof(drv));
            memset(&pci, 0, sizeof(pci));
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            pci.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
            tail = &p2.pNext;
            if (have_driver) {
                *tail = &drv;
                tail = &drv.pNext;
            }
            if (have_pci) {
                *tail = &pci;
            }
            get_props2(pds[i], &p2);

            if (have_driver && drv.driverName[0]) {
                d->driver = g_strdup_printf("%s %s", drv.driverName,
                                            drv.driverInfo);
                g_strstrip(d->driver);
                if (drv.driverInfo[0]) {
                    d->driver_info = g_strdup(drv.driverInfo);
                }
                d->radv = (drv.driverID == VK_DRIVER_ID_MESA_RADV);
            }
            if (have_pci) {
                gchar *slot = g_strdup_printf("%04x:%02x:%02x.%x",
                                              pci.pciDomain, pci.pciBus,
                                              pci.pciDevice, pci.pciFunction);
                gchar *card = drm_card_for_pci_slot(slot);
                if (card) {
                    d->gpu = drm_gpu_name(card);
                }
                g_free(card);
                g_free(slot);
            }
        }
        if (!d->driver) {
            d->driver = g_strdup_printf("driver version 0x%08x",
                                        props.driverVersion);
        }

        g_free(exts);
        *devices = g_list_append(*devices, d);
    }

    g_free(pds);
    if (destroy_instance) destroy_instance(instance, NULL);
    /* Keep the loader mapped: some ICDs register atexit handlers. */
    return TRUE;
}

#else /* !CTK_HAVE_VULKAN_HEADERS */

static gboolean probe_vulkan(gchar **loader_version, GList **devices,
                             gchar **error)
{
    *loader_version = NULL;
    *devices = NULL;
    *error = NULL;
    return FALSE;
}

#endif


static gint compare_vkdev(gconstpointer a, gconstpointer b)
{
    const VkDev *x = a, *y = b;

    /* PCI GPUs first, in GPU_N order, then software devices */
    if (!x->gpu || !y->gpu) {
        return (x->gpu == NULL) - (y->gpu == NULL);
    }
    if (strlen(x->gpu) != strlen(y->gpu)) {
        return strlen(x->gpu) - strlen(y->gpu);
    }
    return strcmp(x->gpu, y->gpu);
}


/* "Mesa 25.2.4 (git-abc)" -> "Mesa 25.2.4" */
static gchar *mesa_version(GList *devices)
{
    GList *l;

    for (l = devices; l; l = l->next) {
        VkDev *d = l->data;
        const gchar *m;
        if (!d->radv || !d->driver_info) continue;
        m = strstr(d->driver_info, "Mesa");
        if (m) {
            const gchar *end = strstr(m, " (");
            return end ? g_strndup(m, end - m) : g_strdup(m);
        }
    }
    return NULL;
}


/* ---- AMD ---------------------------------------------------------------- */

static gchar *x_display_driver(void)
{
    Display *dpy = ctk_amd_display_get_xdisplay();
    Window root;
    XRRScreenResources *res;
    XRRProviderResources *pr;
    GString *s;
    int i, major = 0, minor = 0;

    if (!dpy) {
        return NULL;
    }
    XRRQueryVersion(dpy, &major, &minor);
    if (major < 1 || (major == 1 && minor < 4)) {
        return NULL;
    }
    root = DefaultRootWindow(dpy);
    res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return NULL;
    pr = XRRGetProviderResources(dpy, root);
    s = g_string_new(NULL);

    for (i = 0; pr && i < pr->nproviders; i++) {
        XRRProviderInfo *pi = XRRGetProviderInfo(dpy, res, pr->providers[i]);
        const gchar *desc = NULL;

        if (!pi) continue;
        if (!pi->name || g_ascii_strncasecmp(pi->name, "NVIDIA", 6) == 0) {
            XRRFreeProviderInfo(pi);
            continue;
        }
        if (strcmp(pi->name, "modesetting") == 0) {
            desc = "modesetting (generic KMS driver built into the X server)";
        } else if (g_ascii_strncasecmp(pi->name, "AMDGPU", 6) == 0) {
            desc = "xf86-video-amdgpu";
        } else if (g_ascii_strncasecmp(pi->name, "radeon", 6) == 0) {
            desc = "xf86-video-ati (radeon)";
        }
        if (s->len) g_string_append(s, ", ");
        if (desc) {
            g_string_append(s, desc);
        } else {
            g_string_append(s, pi->name);
        }
        XRRFreeProviderInfo(pi);
    }
    if (pr) XRRFreeProviderResources(pr);
    XRRFreeScreenResources(res);

    if (!s->len) {
        g_string_free(s, TRUE);
        return NULL;
    }
    return g_string_free(s, FALSE);
}


static void add_amd_section(GtkWidget *vbox, GList *amd_gpus,
                            GList *vk_devices, gboolean have_vulkan)
{
    GtkWidget *table = ctk_section_table_new(vbox, "AMD Driver Information");
    struct utsname uts;
    gchar *s, *mod, *mesa;
    gint r = 0;
    GList *l;

    mod = amdgpu_read_string("/sys/module/amdgpu", "version");
    if (uname(&uts) == 0) {
        s = mod ? g_strdup_printf("amdgpu %s (Linux %s)", mod, uts.release)
                : g_strdup_printf("amdgpu (Linux %s)", uts.release);
    } else {
        s = g_strdup("amdgpu");
    }
    row(table, &r, "Kernel Driver:", s);
    g_free(s);
    g_free(mod);

    s = x_display_driver();
    row(table, &r, "X Display Driver:",
        s ? s : (ctk_amd_display_get_xdisplay() ? "Unknown"
                                                : "None (not running on X11)"));
    g_free(s);

    mesa = mesa_version(vk_devices);
    row(table, &r, "Mesa Version:",
        mesa ? mesa : have_vulkan ? "Unknown (no RADV Vulkan driver)"
                                  : "Unknown");
    g_free(mesa);

    for (l = amd_gpus; l; l = l->next) {
        AmdGpu *gpu = l->data;
        gchar *name = drm_gpu_name(gpu->card_name);
        gchar *vbios = amdgpu_read_string(gpu->device_path, "vbios_version");
        gchar *label = g_strdup_printf("%s VBIOS:", name);

        row(table, &r, label, vbios ? vbios : "Unknown");
        g_free(label);
        g_free(vbios);
        g_free(name);
    }
}


/* ---- entry points ------------------------------------------------------- */

void ctk_sysinfo_add_sections(GtkWidget *vbox, gboolean *have_vulkan,
                              gboolean *have_amd)
{
    gchar *loader = NULL, *error = NULL;
    GList *devices = NULL, *amd_gpus, *l;

    *have_vulkan = probe_vulkan(&loader, &devices, &error);
    devices = g_list_sort(devices, compare_vkdev);

    if (*have_vulkan) {
        GtkWidget *table = ctk_section_table_new(vbox, "Vulkan Information");
        gint r = 0;

        row(table, &r, "Vulkan Loader Version:", loader);
        if (error) {
            row(table, &r, "Vulkan Devices:", error);
        }
        for (l = devices; l; l = l->next) {
            VkDev *d = l->data;
            gchar *name = d->gpu ? g_strdup_printf("%s:", d->gpu)
                                 : g_strdup("Software:");
            gchar *value = g_strdup_printf("%s\nVulkan %s, %s, %s", d->name,
                                           d->api, d->driver, d->type);
            row(table, &r, name, value);
            g_free(value);
            g_free(name);
        }
    }

    amd_gpus = amdgpu_enumerate();
    *have_amd = (amd_gpus != NULL);
    if (amd_gpus) {
        add_amd_section(vbox, amd_gpus, devices, *have_vulkan);
    }

    amdgpu_free_list(amd_gpus);
    g_list_free_full(devices, vkdev_free);
    g_free(loader);
    g_free(error);
}


void ctk_sysinfo_add_help(GtkTextBuffer *b, GtkTextIter *i,
                          gboolean have_vulkan, gboolean have_amd)
{
    if (have_vulkan) {
        ctk_help_heading(b, i, "Vulkan Loader Version");
        ctk_help_para(b, i, "The highest Vulkan API version supported by the "
                      "installed Vulkan loader (libvulkan.so.1).");
        ctk_help_heading(b, i, "Vulkan Devices");
        ctk_help_para(b, i, "Each Vulkan device with the highest Vulkan API "
                      "version its driver supports, the driver name and "
                      "version, and the device type.  GPUs are labelled "
                      "GPU_N to match the GPU pages.  If nvidia-settings is "
                      "started with __VK_LAYER_NV_optimus=NVIDIA_only, only "
                      "the NVIDIA GPU is listed.");
    }
    if (have_amd) {
        ctk_help_heading(b, i, "Kernel Driver");
        ctk_help_para(b, i, "The amdgpu kernel driver and the Linux kernel "
                      "it is part of.  An out-of-tree module (such as "
                      "amdgpu-dkms) also reports its module version.");
        ctk_help_heading(b, i, "X Display Driver");
        ctk_help_para(b, i, "The X driver that drives the AMD GPU's "
                      "displays, from its XRandR provider: the generic "
                      "modesetting driver or xf86-video-amdgpu.");
        ctk_help_heading(b, i, "Mesa Version");
        ctk_help_para(b, i, "The version of Mesa, which provides the "
                      "OpenGL (radeonsi) and Vulkan (RADV) drivers for AMD "
                      "GPUs, as reported by the RADV Vulkan driver.");
        ctk_help_heading(b, i, "VBIOS");
        ctk_help_para(b, i, "The video BIOS version of each AMD GPU.");
    }
}
