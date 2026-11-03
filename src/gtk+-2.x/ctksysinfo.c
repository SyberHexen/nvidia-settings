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
#include <stdio.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
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

#if defined(__has_include)
# if __has_include(<EGL/egl.h>) && __has_include(<EGL/eglext.h>)
#  define CTK_HAVE_EGL_HEADERS 1
# endif
#endif

#ifdef CTK_HAVE_EGL_HEADERS
# define EGL_NO_PROTOTYPES
# include <EGL/egl.h>
# include <EGL/eglext.h>
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


/* ---- OpenGL (EGL device platform) --------------------------------------- */

typedef struct {
    gchar *gpu;          /* "GPU_0", or NULL for a software device */
    gchar *renderer;     /* GL_RENDERER */
    gchar *version;      /* GL_VERSION */
    gchar *glsl;         /* GL_SHADING_LANGUAGE_VERSION */
} GlDev;


static void gldev_free(gpointer data)
{
    GlDev *d = data;
    g_free(d->gpu);
    g_free(d->renderer);
    g_free(d->version);
    g_free(d->glsl);
    g_free(d);
}


#ifdef CTK_HAVE_EGL_HEADERS

#define CTK_GL_VENDOR                   0x1F00
#define CTK_GL_RENDERER                 0x1F01
#define CTK_GL_VERSION                  0x1F02
#define CTK_GL_SHADING_LANGUAGE_VERSION 0x8B8C

typedef const unsigned char *(*CtkGlGetStringFn)(unsigned int);

/*
 * Mesa's EGL prints warnings while it probes GPUs it has no driver for
 * (e.g. "failed to create dri2 screen" for the NVIDIA GPU).  They are
 * expected here, so silence stderr around device enumeration and display
 * initialization.
 */
static int quiet_stderr(void)
{
    int saved, devnull;

    fflush(stderr);
    saved = dup(STDERR_FILENO);
    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (saved >= 0 && devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
    }
    if (devnull >= 0) close(devnull);
    return saved;
}

static void restore_stderr(int saved)
{
    if (saved >= 0) {
        fflush(stderr);
        dup2(saved, STDERR_FILENO);
        close(saved);
    }
}

static gboolean has_word(const char *list, const char *word)
{
    size_t len = strlen(word);
    const char *p = list;

    while (p && (p = strstr(p, word))) {
        if ((p == list || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
            return TRUE;
        }
        p += len;
    }
    return FALSE;
}


/*
 * Create an OpenGL context on every EGL device and read its strings.  The
 * EGL device platform addresses each GPU directly, so this works for the
 * render offload GPU as well as the display GPU without any environment
 * variables.  Returns FALSE when EGL device enumeration is unavailable.
 */
static gboolean probe_opengl(GList **devices)
{
    void *h;
    PFNEGLGETPROCADDRESSPROC get_proc;
    PFNEGLQUERYDEVICESEXTPROC query_devices;
    PFNEGLQUERYDEVICESTRINGEXTPROC query_device_string;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display;
    PFNEGLINITIALIZEPROC initialize;
    PFNEGLTERMINATEPROC terminate;
    PFNEGLQUERYSTRINGPROC query_string;
    PFNEGLBINDAPIPROC bind_api;
    PFNEGLCHOOSECONFIGPROC choose_config;
    PFNEGLCREATECONTEXTPROC create_context;
    PFNEGLDESTROYCONTEXTPROC destroy_context;
    PFNEGLCREATEPBUFFERSURFACEPROC create_pbuffer;
    PFNEGLDESTROYSURFACEPROC destroy_surface;
    PFNEGLMAKECURRENTPROC make_current;
    PFNEGLRELEASETHREADPROC release_thread;
    CtkGlGetStringFn get_string;
    const char *client_exts;
    EGLDeviceEXT *devs;
    EGLint n = 0, i;
    int saved_stderr;

    *devices = NULL;

    h = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        return FALSE;
    }
#define SYM(var, type, name) var = (type) dlsym(h, name)
    SYM(get_proc, PFNEGLGETPROCADDRESSPROC, "eglGetProcAddress");
    SYM(initialize, PFNEGLINITIALIZEPROC, "eglInitialize");
    SYM(terminate, PFNEGLTERMINATEPROC, "eglTerminate");
    SYM(query_string, PFNEGLQUERYSTRINGPROC, "eglQueryString");
    SYM(bind_api, PFNEGLBINDAPIPROC, "eglBindAPI");
    SYM(choose_config, PFNEGLCHOOSECONFIGPROC, "eglChooseConfig");
    SYM(create_context, PFNEGLCREATECONTEXTPROC, "eglCreateContext");
    SYM(destroy_context, PFNEGLDESTROYCONTEXTPROC, "eglDestroyContext");
    SYM(create_pbuffer, PFNEGLCREATEPBUFFERSURFACEPROC,
        "eglCreatePbufferSurface");
    SYM(destroy_surface, PFNEGLDESTROYSURFACEPROC, "eglDestroySurface");
    SYM(make_current, PFNEGLMAKECURRENTPROC, "eglMakeCurrent");
    SYM(release_thread, PFNEGLRELEASETHREADPROC, "eglReleaseThread");
#undef SYM
    if (!get_proc || !initialize || !terminate || !query_string ||
        !bind_api || !choose_config || !create_context || !destroy_context ||
        !create_pbuffer || !destroy_surface || !make_current) {
        return FALSE;
    }

    client_exts = query_string(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!client_exts || !has_word(client_exts, "EGL_EXT_device_enumeration") ||
        !has_word(client_exts, "EGL_EXT_platform_device")) {
        return FALSE;
    }
    query_devices = (PFNEGLQUERYDEVICESEXTPROC)
        get_proc("eglQueryDevicesEXT");
    query_device_string = (PFNEGLQUERYDEVICESTRINGEXTPROC)
        get_proc("eglQueryDeviceStringEXT");
    get_platform_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)
        get_proc("eglGetPlatformDisplayEXT");
    get_string = (CtkGlGetStringFn) get_proc("glGetString");
    if (!query_devices || !query_device_string || !get_platform_display ||
        !get_string) {
        return FALSE;
    }
    saved_stderr = quiet_stderr();
    if (!query_devices(0, NULL, &n) || n <= 0) {
        restore_stderr(saved_stderr);
        return FALSE;
    }
    devs = g_new0(EGLDeviceEXT, n);
    if (!query_devices(n, devs, &n)) {
        n = 0;
    }
    restore_stderr(saved_stderr);

    for (i = 0; i < n; i++) {
        const char *dexts = query_device_string(devs[i], EGL_EXTENSIONS);
        const char *file = NULL;
        EGLDisplay dpy;
        EGLint major, minor, nconf = 0;
        EGLConfig config = NULL;
        EGLContext ctx;
        EGLSurface surf = EGL_NO_SURFACE;
        gboolean surfaceless, no_config, ok;
        const char *dpy_exts;
        gchar *gpu = NULL;
        GList *l;
        static const EGLint pbuf_attrs[] = {
            EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE
        };
        EGLint conf_attrs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_NONE
        };

        if (dexts && has_word(dexts, "EGL_EXT_device_drm")) {
            file = query_device_string(devs[i], EGL_DRM_DEVICE_FILE_EXT);
        }
        if (file) {
            gchar *card = g_path_get_basename(file);
            gpu = drm_gpu_name(card);
            g_free(card);
        } else if (!dexts || !has_word(dexts, "EGL_MESA_device_software")) {
            continue;   /* neither a DRM GPU nor a software renderer */
        }

        /* Several EGL vendors may expose the same GPU; keep the first one
         * that can create a context. */
        for (l = *devices; l; l = l->next) {
            if (g_strcmp0(((GlDev *) l->data)->gpu, gpu) == 0) break;
        }
        if (l) {
            g_free(gpu);
            continue;
        }

        saved_stderr = quiet_stderr();
        dpy = get_platform_display(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
        ok = (dpy != EGL_NO_DISPLAY && initialize(dpy, &major, &minor));
        restore_stderr(saved_stderr);
        if (!ok) {
            g_free(gpu);
            continue;
        }
        dpy_exts = query_string(dpy, EGL_EXTENSIONS);
        surfaceless = dpy_exts &&
                      has_word(dpy_exts, "EGL_KHR_surfaceless_context");
        /* Device displays usually have no configs at all; contexts need
         * EGL_KHR_no_config_context there. */
        no_config = surfaceless && dpy_exts &&
                    has_word(dpy_exts, "EGL_KHR_no_config_context");
        if (surfaceless) {
            conf_attrs[2] = EGL_NONE;   /* any surface type */
        }
        if (no_config) {
            config = EGL_NO_CONFIG_KHR;
            ok = TRUE;
        } else {
            ok = choose_config(dpy, conf_attrs, &config, 1, &nconf) &&
                 nconf > 0;
        }

        if (ok && bind_api(EGL_OPENGL_API) &&
            (ctx = create_context(dpy, config, EGL_NO_CONTEXT, NULL))
                != EGL_NO_CONTEXT) {

            if (!surfaceless) {
                surf = create_pbuffer(dpy, config, pbuf_attrs);
            }
            if ((surfaceless || surf != EGL_NO_SURFACE) &&
                make_current(dpy, surf, surf, ctx)) {
                const unsigned char *r = get_string(CTK_GL_RENDERER);
                const unsigned char *v = get_string(CTK_GL_VERSION);
                const unsigned char *g =
                    get_string(CTK_GL_SHADING_LANGUAGE_VERSION);

                if (r && v) {
                    GlDev *d = g_new0(GlDev, 1);
                    d->gpu = gpu;
                    gpu = NULL;
                    d->renderer = g_strdup((const char *) r);
                    d->version = g_strdup((const char *) v);
                    d->glsl = g ? g_strdup((const char *) g) : NULL;
                    *devices = g_list_append(*devices, d);
                }
                make_current(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                             EGL_NO_CONTEXT);
            }
            if (surf != EGL_NO_SURFACE) {
                destroy_surface(dpy, surf);
            }
            destroy_context(dpy, ctx);
        }
        terminate(dpy);
        g_free(gpu);
    }

    g_free(devs);
    if (release_thread) {
        release_thread();
    }
    /* Keep libEGL mapped: vendor libraries may register atexit handlers. */
    return TRUE;
}

#else /* !CTK_HAVE_EGL_HEADERS */

static gboolean probe_opengl(GList **devices)
{
    *devices = NULL;
    return FALSE;
}

#endif


/* Sort by GPU_N; both VkDev and GlDev start with their "GPU_N" name. */
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
static gchar *mesa_version(GList *devices, GList *gl_devices)
{
    GList *l;
    const gchar *m = NULL, *end;

    /* Prefer RADV's version, then any Mesa OpenGL driver on a real GPU. */
    for (l = devices; l && !m; l = l->next) {
        VkDev *d = l->data;
        if (d->radv && d->driver_info) {
            m = strstr(d->driver_info, "Mesa");
        }
    }
    for (l = gl_devices; l && !m; l = l->next) {
        GlDev *d = l->data;
        if (d->gpu && d->version) {
            m = strstr(d->version, "Mesa ");
        }
    }
    if (!m) {
        return NULL;
    }
    end = strstr(m, " (");
    return end ? g_strndup(m, end - m) : g_strdup(m);
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
                            GList *vk_devices, gboolean have_vulkan,
                            GList *gl_devices)
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

    mesa = mesa_version(vk_devices, gl_devices);
    row(table, &r, "Mesa Version:", mesa ? mesa : "Unknown");
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
                              gboolean *have_gl, gboolean *have_amd)
{
    gchar *loader = NULL, *error = NULL;
    GList *devices = NULL, *gl_devices = NULL, *amd_gpus, *l;

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

    *have_gl = probe_opengl(&gl_devices);
    gl_devices = g_list_sort(gl_devices, compare_vkdev);
    if (*have_gl && gl_devices) {
        GtkWidget *table = ctk_section_table_new(vbox, "OpenGL Information");
        gint r = 0;

        for (l = gl_devices; l; l = l->next) {
            GlDev *d = l->data;
            gchar *name = d->gpu ? g_strdup_printf("%s:", d->gpu)
                                 : g_strdup("Software:");
            gchar *value = d->glsl ?
                g_strdup_printf("%s\nOpenGL %s, GLSL %s", d->renderer,
                                d->version, d->glsl) :
                g_strdup_printf("%s\nOpenGL %s", d->renderer, d->version);
            row(table, &r, name, value);
            g_free(value);
            g_free(name);
        }
    } else {
        *have_gl = FALSE;
    }

    amd_gpus = amdgpu_enumerate();
    *have_amd = (amd_gpus != NULL);
    if (amd_gpus) {
        add_amd_section(vbox, amd_gpus, devices, *have_vulkan, gl_devices);
    }

    amdgpu_free_list(amd_gpus);
    g_list_free_full(gl_devices, gldev_free);
    g_list_free_full(devices, vkdev_free);
    g_free(loader);
    g_free(error);
}


void ctk_sysinfo_add_help(GtkTextBuffer *b, GtkTextIter *i,
                          gboolean have_vulkan, gboolean have_gl,
                          gboolean have_amd)
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
    if (have_gl) {
        ctk_help_heading(b, i, "OpenGL Devices");
        ctk_help_para(b, i, "The OpenGL renderer, version and GLSL version "
                      "of each GPU.  Each GPU is queried directly through "
                      "the EGL device platform, so this includes the PRIME "
                      "render offload GPU, which applications only use when "
                      "they are offloaded to it.  The version is that of a "
                      "default (compatibility profile) context.");
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
