/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * PRIME render offload / reverse PRIME page.
 *
 * With PRIME render offload another GPU (typically an integrated AMD or
 * Intel GPU) drives the X screen and the NVIDIA GPU only renders the
 * applications that are explicitly offloaded to it.  There is no NVIDIA X
 * screen in that configuration, so NV-CONTROL has nothing to report about
 * PRIME (NV_CTRL_STRING_PRIME_OUTPUTS_DATA is an X screen attribute that
 * only describes reverse PRIME displays of an NVIDIA X screen).  This page
 * gathers the state from XRandR providers, NVML and sysfs instead.
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
#include <unistd.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

#include "nvml.h"

#include "ctkprime.h"
#include "displaycfg.h"
#include "ctkbanner.h"
#include "ctkhelp.h"
#include "ctkutils.h"
#include "amdgpu-sysfs.h"

#define PRIME_UPDATE_INTERVAL_MS 2000
#define NVIDIA_PCI_VENDOR_ID 0x10de
#define NV_GPU_SCREEN_PREFIX "NVIDIA-G"

enum {
    PROC_COL_PID,
    PROC_COL_NAME,
    PROC_COL_TYPE,
    PROC_COL_MEMORY,
    PROC_COL_GPU,
    PROC_NUM_COLS
};


static const char *__mode_help =
"How the GPUs are combined.  With render offload the display GPU drives the "
"X screen and the NVIDIA GPU renders only the applications that are "
"offloaded to it.  With reverse PRIME the NVIDIA GPU additionally scans out "
"displays connected to it, showing an image rendered by the display GPU.";

static const char *__offload_help =
"Render offload requires the X server to create an NVIDIA \"GPU screen\", "
"which is visible as an XRandR provider named NVIDIA-G0.  The X server only "
"does this when Option \"AllowNVIDIAGPUScreens\" is set in the ServerLayout "
"section (most distributions install a configuration snippet that sets it).";

static const char *__vulkan_help =
"Vulkan applications are offloaded by the VK_LAYER_NV_optimus implicit "
"layer, which is installed with the NVIDIA driver.";

static const char *__power_help =
"The PCI runtime power state of the NVIDIA GPU.  With runtime power "
"management enabled the GPU is powered down while no application uses it.  "
"The application list below is not refreshed while the GPU is suspended so "
"that this page does not wake it up.";

static const char *__providers_help =
"The XRandR providers of the X server: one per GPU that takes part in the "
"X screen.  'Source Output' providers render images that 'Sink Output' "
"providers display; 'Source Offload' providers render for 'Sink Offload' "
"providers.";

static const char *__procs_help =
"Applications that currently have a graphics or compute context on the "
"NVIDIA GPU, as reported by NVML, refreshed every two seconds while this "
"page is visible.  With render offload the X server always holds a small "
"context for its NVIDIA GPU screen.";

static const char *__launch_help =
"Starts a program with the environment variables that offload its OpenGL "
"(GLX/EGL) and Vulkan rendering to the NVIDIA GPU.  The program's windows "
"are still shown by the display GPU.  The same variables can be used from "
"a shell, a .desktop file, or a game launcher such as Steam.";

static const char *__vk_only_help =
"Sets __VK_LAYER_NV_optimus=NVIDIA_only so Vulkan applications only see the "
"NVIDIA GPU.  Without it they see every GPU and may pick the display GPU.";

static const char *__rprime_help =
"Reverse PRIME lets displays connected to the NVIDIA GPU extend the X "
"screen of the display GPU.  It requires the NVIDIA provider to use the "
"display GPU as its output source (xrandr --setprovideroutputsource).  "
"Connected displays then appear as pages below this one, where they can "
"be enabled and configured.";


GType ctk_prime_get_type(void)
{
    static GType type = 0;

    if (!type) {
        static const GTypeInfo info = {
            sizeof (CtkPrimeClass),
            NULL, NULL, NULL, NULL, NULL,
            sizeof (CtkPrime),
            0, NULL, NULL
        };
        type = g_type_register_static(GTK_TYPE_VBOX, "CtkPrime", &info, 0);
    }
    return type;
}


void ctk_prime_gpu_free(gpointer data)
{
    CtkPrimeGpu *gpu = data;
    if (!gpu) return;
    g_free(gpu->pci_slot);
    g_free(gpu->card);
    g_free(gpu->label);
    g_free(gpu);
}


void ctk_prime_card_name_free(gpointer data)
{
    CtkPrimeCardName *n = data;
    if (!n) return;
    g_free(n->card);
    g_free(n->label);
    g_free(n);
}


/* ---- X helpers ---------------------------------------------------------- */

static void trap_push(void)
{
#if GTK_CHECK_VERSION(3, 0, 0)
    gdk_x11_display_error_trap_push(gdk_display_get_default());
#else
    gdk_error_trap_push();
#endif
}

static gint trap_pop(void)
{
#if GTK_CHECK_VERSION(3, 0, 0)
    return gdk_x11_display_error_trap_pop(gdk_display_get_default());
#else
    gdk_flush();
    return gdk_error_trap_pop();
#endif
}


static gboolean have_randr_providers(Display *dpy)
{
    int ev, err, major = 0, minor = 0;

    return dpy && XRRQueryExtension(dpy, &ev, &err) &&
           XRRQueryVersion(dpy, &major, &minor) &&
           (major > 1 || (major == 1 && minor >= 4));
}


static gboolean is_nv_gpu_screen(const XRRProviderInfo *pi)
{
    return pi->name && g_str_has_prefix(pi->name, NV_GPU_SCREEN_PREFIX);
}


static gboolean is_nvidia_provider(const XRRProviderInfo *pi)
{
    return pi->name && g_ascii_strncasecmp(pi->name, "NVIDIA", 6) == 0;
}


static gboolean output_connector_id(Display *dpy, RROutput out, long *id)
{
    Atom atom = XInternAtom(dpy, "CONNECTOR_ID", True);
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    gboolean ok = FALSE;

    if (atom == None) {
        return FALSE;
    }
    if (XRRGetOutputProperty(dpy, out, atom, 0, 1, False, False, XA_INTEGER,
                             &type, &format, &n, &after, &data) == Success &&
        type == XA_INTEGER && n >= 1 && format == 32) {
        *id = ((long *) data)[0];
        ok = TRUE;
    }
    if (data) XFree(data);
    return ok;
}


static gchar *output_monitor_name(Display *dpy, RROutput out)
{
    Atom atom = XInternAtom(dpy, "EDID", True);
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    gchar *name = NULL;

    if (atom == None) {
        return NULL;
    }
    if (XRRGetOutputProperty(dpy, out, atom, 0, 256, False, False,
                             AnyPropertyType, &type, &format, &n, &after,
                             &data) == Success && format == 8 && n >= 128) {
        name = amdgpu_edid_monitor_name(data, n);
    }
    if (data) XFree(data);
    return name;
}


GList *ctk_prime_find_nvidia_outputs(Display *dpy)
{
    Window root;
    XRRScreenResources *res;
    XRRProviderResources *pr;
    GList *list = NULL;
    int i, j;

    if (!have_randr_providers(dpy)) {
        return NULL;
    }
    root = DefaultRootWindow(dpy);
    res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) {
        return NULL;
    }
    pr = XRRGetProviderResources(dpy, root);
    for (i = 0; pr && i < pr->nproviders; i++) {
        XRRProviderInfo *pi = XRRGetProviderInfo(dpy, res, pr->providers[i]);
        if (!pi) continue;
        if (is_nv_gpu_screen(pi)) {
            for (j = 0; j < pi->noutputs; j++) {
                XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, pi->outputs[j]);
                if (oi && oi->connection == RR_Connected) {
                    list = g_list_append(list,
                                         GUINT_TO_POINTER(pi->outputs[j]));
                }
                if (oi) XRRFreeOutputInfo(oi);
            }
        }
        XRRFreeProviderInfo(pi);
    }
    if (pr) XRRFreeProviderResources(pr);
    XRRFreeScreenResources(res);
    return list;
}


/* ---- DRM cards ---------------------------------------------------------- */

typedef struct {
    gchar *name;            /* "card1" */
    guint vendor;
    gchar *driver;          /* "amdgpu" */
    GHashTable *conn_ids;   /* DRM connector ids */
} DrmCard;


static void drm_card_free(gpointer data)
{
    DrmCard *c = data;
    g_free(c->name);
    g_free(c->driver);
    g_hash_table_destroy(c->conn_ids);
    g_free(c);
}


static gboolean is_card_name(const gchar *s)
{
    const gchar *p;

    if (!g_str_has_prefix(s, "card")) return FALSE;
    for (p = s + 4; *p; p++) {
        if (!g_ascii_isdigit(*p)) return FALSE;
    }
    return p != s + 4;
}


static GList *enumerate_cards(void)
{
    GDir *dir = g_dir_open("/sys/class/drm", 0, NULL);
    const gchar *e;
    GList *cards = NULL, *l;

    if (!dir) {
        return NULL;
    }
    while ((e = g_dir_read_name(dir))) {
        gchar *dev, *link, *drv;
        gint64 v;
        DrmCard *c;

        if (!is_card_name(e)) continue;
        c = g_new0(DrmCard, 1);
        c->name = g_strdup(e);
        c->conn_ids = g_hash_table_new(g_direct_hash, g_direct_equal);
        dev = g_build_filename("/sys/class/drm", e, "device", NULL);
        c->vendor = amdgpu_read_int64(dev, "vendor", &v) ? (guint) v : 0;
        link = g_build_filename(dev, "driver", NULL);
        drv = g_file_read_link(link, NULL);
        c->driver = drv ? g_path_get_basename(drv) : g_strdup("unknown");
        g_free(drv);
        g_free(link);
        g_free(dev);
        cards = g_list_append(cards, c);
    }

    /* connectors are named "cardN-<connector>" */
    g_dir_rewind(dir);
    while ((e = g_dir_read_name(dir))) {
        const gchar *dash = strchr(e, '-');
        gchar *card, *path;
        gint64 id;

        if (!dash) continue;
        card = g_strndup(e, dash - e);
        path = g_build_filename("/sys/class/drm", e, NULL);
        if (is_card_name(card) &&
            amdgpu_read_int64(path, "connector_id", &id)) {
            for (l = cards; l; l = l->next) {
                DrmCard *c = l->data;
                if (strcmp(c->name, card) == 0) {
                    g_hash_table_insert(c->conn_ids,
                                        GINT_TO_POINTER((gint) id),
                                        GINT_TO_POINTER(1));
                }
            }
        }
        g_free(path);
        g_free(card);
    }
    g_dir_close(dir);
    return cards;
}


/* Match an XRandR provider to the DRM card it belongs to */
static DrmCard *provider_card(Display *dpy, GList *cards,
                              XRRProviderInfo *pi)
{
    gboolean nv = is_nvidia_provider(pi);
    DrmCard *best = NULL, *only = NULL;
    gint best_score = 0, ncand = 0;
    GList *l;

    for (l = cards; l; l = l->next) {
        DrmCard *c = l->data;
        gint score = 0, j;

        if ((c->vendor == NVIDIA_PCI_VENDOR_ID) != nv) continue;
        ncand++;
        only = c;
        for (j = 0; j < pi->noutputs; j++) {
            long id;
            if (output_connector_id(dpy, pi->outputs[j], &id) &&
                g_hash_table_lookup(c->conn_ids, GINT_TO_POINTER((gint) id))) {
                score++;
            }
        }
        if (score > best_score) {
            best_score = score;
            best = c;
        }
    }
    if (best) return best;
    return ncand == 1 ? only : NULL;
}


static gchar *card_label(CtkPrime *page, const DrmCard *card)
{
    GList *l;

    if (!card) {
        return g_strdup("Unknown");
    }
    for (l = page->nv_gpus; l; l = l->next) {
        CtkPrimeGpu *g = l->data;
        if (g->card && strcmp(g->card, card->name) == 0) {
            return g_strdup(g->label);
        }
    }
    for (l = page->card_names; l; l = l->next) {
        CtkPrimeCardName *n = l->data;
        if (strcmp(n->card, card->name) == 0) {
            return g_strdup(n->label);
        }
    }
    {
        gchar *name = drm_gpu_name(card->name);
        gchar *ret = g_strdup_printf("%s - (%s)", name, card->driver);
        g_free(name);
        return ret;
    }
}


/* ---- NVML --------------------------------------------------------------- */

typedef nvmlReturn_t (*nvmlInitFn)(void);
typedef nvmlReturn_t (*nvmlGetByPciFn)(const char *, nvmlDevice_t *);
typedef nvmlReturn_t (*nvmlProcsFn)(nvmlDevice_t, unsigned int *,
                                    nvmlProcessInfo_t *);
typedef const char * (*nvmlErrorStringFn)(nvmlReturn_t);

static struct {
    gboolean tried;
    gboolean ok;
    gchar *error;
    nvmlGetByPciFn get_by_pci;
    nvmlProcsFn graphics_procs;
    nvmlProcsFn compute_procs;
    nvmlErrorStringFn error_string;
} nvml;


static gboolean nvml_load(void)
{
    void *h;
    nvmlInitFn init;
    nvmlReturn_t ret;

    if (nvml.tried) {
        return nvml.ok;
    }
    nvml.tried = TRUE;

    h = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!h) {
        nvml.error = g_strdup_printf("Unable to load libnvidia-ml.so.1 (%s).",
                                     dlerror());
        return FALSE;
    }
    init = (nvmlInitFn) dlsym(h, "nvmlInit_v2");
    nvml.get_by_pci =
        (nvmlGetByPciFn) dlsym(h, "nvmlDeviceGetHandleByPciBusId_v2");
    nvml.graphics_procs =
        (nvmlProcsFn) dlsym(h, "nvmlDeviceGetGraphicsRunningProcesses_v3");
    nvml.compute_procs =
        (nvmlProcsFn) dlsym(h, "nvmlDeviceGetComputeRunningProcesses_v3");
    nvml.error_string = (nvmlErrorStringFn) dlsym(h, "nvmlErrorString");

    if (!init || !nvml.get_by_pci || !nvml.graphics_procs ||
        !nvml.compute_procs) {
        nvml.error = g_strdup("The installed NVML library is too old.");
        dlclose(h);
        return FALSE;
    }
    ret = init();
    if (ret != NVML_SUCCESS) {
        nvml.error = g_strdup_printf("NVML initialization failed (%s).",
                                     nvml.error_string ?
                                     nvml.error_string(ret) : "unknown error");
        dlclose(h);
        return FALSE;
    }
    /* Keep NVML loaded and initialized for the lifetime of the process. */
    nvml.ok = TRUE;
    return TRUE;
}


static nvmlProcessInfo_t *nvml_get_procs(nvmlProcsFn fn, nvmlDevice_t dev,
                                         unsigned int *count)
{
    nvmlProcessInfo_t *procs = NULL;
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        unsigned int n = 0;
        nvmlReturn_t ret = fn(dev, &n, NULL);

        if (ret == NVML_SUCCESS) {
            *count = 0;
            return NULL;
        }
        if (ret != NVML_ERROR_INSUFFICIENT_SIZE) {
            break;
        }
        n += 8;    /* processes may start between the two calls */
        procs = g_new0(nvmlProcessInfo_t, n);
        ret = fn(dev, &n, procs);
        if (ret == NVML_SUCCESS) {
            *count = n;
            return procs;
        }
        g_free(procs);
        procs = NULL;
        if (ret != NVML_ERROR_INSUFFICIENT_SIZE) {
            break;
        }
    }
    *count = 0;
    return NULL;
}


/* ---- sysfs helpers ------------------------------------------------------ */

static gchar *pci_power_file(const gchar *slot, const gchar *file)
{
    gchar *dir, *val;

    if (!slot) return NULL;
    dir = g_build_filename("/sys/bus/pci/devices", slot, "power", NULL);
    val = amdgpu_read_string(dir, file);
    g_free(dir);
    return val;
}


static gboolean gpu_suspended(const CtkPrimeGpu *gpu)
{
    gchar *s = pci_power_file(gpu->pci_slot, "runtime_status");
    gboolean ret = (g_strcmp0(s, "suspended") == 0);
    g_free(s);
    return ret;
}


static gchar *find_vulkan_layer(void)
{
    static const gchar *dirs[] = {
        "/etc/vulkan/implicit_layer.d",
        "/usr/local/share/vulkan/implicit_layer.d",
        "/usr/share/vulkan/implicit_layer.d",
        NULL
    };
    gint i;

    for (i = 0; dirs[i]; i++) {
        GDir *dir = g_dir_open(dirs[i], 0, NULL);
        const gchar *e;

        if (!dir) continue;
        while ((e = g_dir_read_name(dir))) {
            gchar *path, *contents = NULL;

            if (!g_str_has_suffix(e, ".json")) continue;
            path = g_build_filename(dirs[i], e, NULL);
            if (g_file_get_contents(path, &contents, NULL, NULL) &&
                strstr(contents, "VK_LAYER_NV_optimus")) {
                g_free(contents);
                g_dir_close(dir);
                return path;
            }
            g_free(contents);
            g_free(path);
        }
        g_dir_close(dir);
    }
    return NULL;
}


/* The command line name of a process: argv[0] without its directory */
static gchar *process_name(guint pid)
{
    gchar *path, *data = NULL, *name = NULL;
    gsize len = 0;

    path = g_strdup_printf("/proc/%u/cmdline", pid);
    if (g_file_get_contents(path, &data, &len, NULL) && len > 0 && data[0]) {
        const gchar *base = data, *p;
        for (p = data; *p; p++) {
            if (*p == '/' || *p == '\\') base = p + 1;
        }
        if (*base) name = g_strdup(base);
    }
    g_free(data);
    g_free(path);

    if (!name) {
        path = g_strdup_printf("/proc/%u/comm", pid);
        if (g_file_get_contents(path, &data, NULL, NULL)) {
            name = g_strdup(g_strstrip(data));
        }
        g_free(data);
        g_free(path);
    }
    return name;
}


static gboolean is_x_server(const gchar *name)
{
    return name && (g_ascii_strcasecmp(name, "Xorg") == 0 ||
                    g_ascii_strcasecmp(name, "Xorg.bin") == 0 ||
                    g_ascii_strcasecmp(name, "Xwayland") == 0 ||
                    strcmp(name, "X") == 0);
}


/* ---- layout helpers ----------------------------------------------------- */

static GtkWidget *new_table(void)
{
    GtkWidget *table = gtk_table_new(1, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(table), 3);
    gtk_table_set_col_spacings(GTK_TABLE(table), 15);
    return table;
}


static GtkWidget *info_row(CtkPrime *page, GtkWidget *table, gint *row,
                           const gchar *name, const gchar *help)
{
    GtkWidget *l;

    gtk_table_resize(GTK_TABLE(table), *row + 1, 2);
    l = add_table_row_with_help_text(table, page->ctk_config, help, *row, 0,
                                     0.0f, 0.5f, name, 0.0f, 0.5f, "");
    (*row)++;
    return l;
}


static GtkWidget *left_label(const gchar *text, gboolean bold)
{
    GtkWidget *l = gtk_label_new(NULL);

    if (bold) {
        gchar *m = g_markup_printf_escaped("<b>%s</b>", text);
        gtk_label_set_markup(GTK_LABEL(l), m);
        g_free(m);
    } else {
        gtk_label_set_text(GTK_LABEL(l), text);
    }
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    return l;
}


static void grid_cell(GtkWidget *table, gint col, gint row,
                      const gchar *text, gboolean bold)
{
    gtk_table_attach(GTK_TABLE(table), left_label(text, bold),
                     col, col + 1, row, row + 1, GTK_FILL, GTK_FILL, 0, 0);
}


static void clear_container(GtkWidget *box)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    GList *l;

    for (l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}


static gchar *caps_string(unsigned int caps)
{
    GString *s = g_string_new(NULL);

    if (caps & RR_Capability_SourceOutput)  g_string_append(s, "Source Output, ");
    if (caps & RR_Capability_SinkOutput)    g_string_append(s, "Sink Output, ");
    if (caps & RR_Capability_SourceOffload) g_string_append(s, "Source Offload, ");
    if (caps & RR_Capability_SinkOffload)   g_string_append(s, "Sink Offload, ");
    if (s->len >= 2) {
        g_string_truncate(s, s->len - 2);
    } else {
        g_string_append(s, "None");
    }
    return g_string_free(s, FALSE);
}


/* ---- launcher ----------------------------------------------------------- */

static gchar **offload_environment(CtkPrime *page, gchar **envp)
{
    envp = g_environ_setenv(envp, "__NV_PRIME_RENDER_OFFLOAD", "1", TRUE);
    envp = g_environ_setenv(envp, "__GLX_VENDOR_LIBRARY_NAME", "nvidia", TRUE);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->vk_only_check))) {
        envp = g_environ_setenv(envp, "__VK_LAYER_NV_optimus", "NVIDIA_only",
                                TRUE);
    }
    if (page->offload_provider) {
        envp = g_environ_setenv(envp, "__NV_PRIME_RENDER_OFFLOAD_PROVIDER",
                                page->offload_provider, TRUE);
    }
    return envp;
}


static void update_env_labels(CtkPrime *page)
{
    gchar **env = offload_environment(page, NULL);
    gchar *joined = g_strjoinv(" ", env);
    gchar *steam = g_strdup_printf("%s %%command%%", joined);

    gtk_label_set_text(GTK_LABEL(page->env_label), joined);
    gtk_label_set_text(GTK_LABEL(page->steam_label), steam);
    g_free(steam);
    g_free(joined);
    g_strfreev(env);
}


static void vk_only_toggled(GtkToggleButton *b, gpointer data)
{
    update_env_labels(CTK_PRIME(data));
}


static void run_clicked(GtkWidget *w, gpointer data)
{
    CtkPrime *page = CTK_PRIME(data);
    const gchar *cmd = gtk_entry_get_text(GTK_ENTRY(page->cmd_entry));
    gchar **argv = NULL, **envp;
    GError *error = NULL;
    GPid pid;

    while (cmd && g_ascii_isspace(*cmd)) {
        cmd++;
    }
    if (!cmd || !*cmd) {
        return;
    }
    if (!g_shell_parse_argv(cmd, NULL, &argv, &error)) {
        gchar *msg = g_strdup_printf("Unable to parse the command: %s",
                                     error->message);
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(page)), msg);
        g_free(msg);
        g_error_free(error);
        return;
    }

    envp = offload_environment(page, g_get_environ());
    /* Without G_SPAWN_DO_NOT_REAP_CHILD GLib reaps the child for us. */
    if (g_spawn_async(NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL,
                      &pid, &error)) {
        ctk_config_statusbar_message(page->ctk_config,
                                     "Started '%s' on the NVIDIA GPU "
                                     "(PID %d).", argv[0], (int) pid);
    } else {
        gchar *msg = g_strdup_printf("Unable to start '%s': %s", argv[0],
                                     error->message);
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(page)), msg);
        g_free(msg);
        g_error_free(error);
    }
    g_strfreev(envp);
    g_strfreev(argv);
}


/* ---- reverse PRIME ------------------------------------------------------ */

static void rprime_clicked(GtkButton *b, gpointer data)
{
    CtkPrime *page = CTK_PRIME(data);
    RRProvider source;
    Window root;
    XRRScreenResources *res;
    gint err;

    if (!page->dpy || page->nv_provider == None) {
        return;
    }
    if (page->rprime_enabled && page->rprime_active) {
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(page)),
                              "Disable the displays connected to the NVIDIA "
                              "GPU before disabling reverse PRIME.");
        return;
    }
    if (!page->rprime_enabled && page->display_provider == None) {
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(page)),
                              "No display GPU provider can act as the output "
                              "source.");
        return;
    }

    source = page->rprime_enabled ? None : page->display_provider;
    root = DefaultRootWindow(page->dpy);
    trap_push();
    res = XRRGetScreenResourcesCurrent(page->dpy, root);
    XRRSetProviderOutputSource(page->dpy, page->nv_provider, source);
    XSync(page->dpy, False);
    if (res) XRRFreeScreenResources(res);
    err = trap_pop();

    if (err) {
        ctk_display_error_msg(ctk_get_parent_window(GTK_WIDGET(page)),
                              "The X server rejected the provider output "
                              "source change.");
    } else {
        ctk_config_statusbar_message(page->ctk_config, "Reverse PRIME %s.",
                                     source ? "enabled" : "disabled");
    }
    /* RandR events also trigger a refresh, but update right away. */
    ctk_prime_refresh(page);
}


/* ---- refresh ------------------------------------------------------------ */

static void update_power(CtkPrime *page)
{
    GString *s = g_string_new(NULL);
    GList *l;

    for (l = page->nv_gpus; l; l = l->next) {
        CtkPrimeGpu *g = l->data;
        gchar *status = pci_power_file(g->pci_slot, "runtime_status");
        gchar *control = pci_power_file(g->pci_slot, "control");

        if (s->len) g_string_append_c(s, '\n');
        if (g_list_length(page->nv_gpus) > 1) {
            gchar *name = g->card ? drm_gpu_name(g->card) : g_strdup(g->label);
            g_string_append_printf(s, "%s: ", name);
            g_free(name);
        }
        g_string_append(s, status ? status : "unknown");
        if (control) {
            g_string_append_printf(s, " (runtime power management %s)",
                                   strcmp(control, "auto") == 0 ?
                                   "enabled" : "disabled");
        }
        g_free(status);
        g_free(control);
    }
    gtk_label_set_text(GTK_LABEL(page->power_label),
                       s->len ? s->str : "Unknown");
    g_string_free(s, TRUE);
}


typedef struct {
    guint pid;
    gboolean graphics, compute;
    unsigned long long mem;
    const CtkPrimeGpu *gpu;
} ProcEntry;


static void collect_procs(GHashTable *table, nvmlProcessInfo_t *procs,
                          unsigned int n, gboolean graphics,
                          const CtkPrimeGpu *gpu, GList **order)
{
    unsigned int i;

    for (i = 0; i < n; i++) {
        ProcEntry *e = g_hash_table_lookup(table,
                                           GUINT_TO_POINTER(procs[i].pid));
        if (!e) {
            e = g_new0(ProcEntry, 1);
            e->pid = procs[i].pid;
            e->mem = procs[i].usedGpuMemory;
            e->gpu = gpu;
            g_hash_table_insert(table, GUINT_TO_POINTER(e->pid), e);
            *order = g_list_append(*order, e);
        } else if (e->mem == (unsigned long long) NVML_VALUE_NOT_AVAILABLE) {
            e->mem = procs[i].usedGpuMemory;
        }
        if (graphics) e->graphics = TRUE; else e->compute = TRUE;
    }
}


static void update_processes(CtkPrime *page)
{
    GList *l;
    gint napps = 0, nsuspended = 0, nqueried = 0;
    gboolean multi = g_list_length(page->nv_gpus) > 1;
    gchar *msg;

    gtk_list_store_clear(page->proc_store);

    if (!nvml_load()) {
        gtk_label_set_text(GTK_LABEL(page->proc_status_label), nvml.error);
        return;
    }

    for (l = page->nv_gpus; l; l = l->next) {
        CtkPrimeGpu *g = l->data;
        nvmlDevice_t dev;
        nvmlProcessInfo_t *procs;
        unsigned int n;
        GHashTable *table;
        GList *order = NULL, *o;

        if (!g->pci_slot) continue;
        if (gpu_suspended(g)) {
            nsuspended++;
            continue;
        }
        if (nvml.get_by_pci(g->pci_slot, &dev) != NVML_SUCCESS) continue;
        nqueried++;

        table = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                      NULL, g_free);
        procs = nvml_get_procs(nvml.graphics_procs, dev, &n);
        collect_procs(table, procs, n, TRUE, g, &order);
        g_free(procs);
        procs = nvml_get_procs(nvml.compute_procs, dev, &n);
        collect_procs(table, procs, n, FALSE, g, &order);
        g_free(procs);

        for (o = order; o; o = o->next) {
            ProcEntry *e = o->data;
            gchar *name = process_name(e->pid);
            gchar *pid = g_strdup_printf("%u", e->pid);
            gchar *mem, *shown, *gpu_col;
            GtkTreeIter it;

            if (e->mem == (unsigned long long) NVML_VALUE_NOT_AVAILABLE) {
                mem = g_strdup("N/A");
            } else {
                mem = g_strdup_printf("%llu MiB", e->mem >> 20);
            }
            if (is_x_server(name)) {
                shown = g_strdup_printf("%s (X server)", name);
            } else if (e->pid == (guint) getpid()) {
                shown = g_strdup_printf("%s (this program)", name);
            } else {
                shown = g_strdup(name ? name : "(unknown)");
                napps++;
            }
            gpu_col = !multi ? g_strdup("") :
                      e->gpu->card ? drm_gpu_name(e->gpu->card) :
                      g_strdup(e->gpu->label);
            gtk_list_store_append(page->proc_store, &it);
            gtk_list_store_set(page->proc_store, &it,
                               PROC_COL_PID, pid,
                               PROC_COL_NAME, shown,
                               PROC_COL_TYPE,
                               e->graphics && e->compute ?
                                   "Graphics + Compute" :
                               e->graphics ? "Graphics" : "Compute",
                               PROC_COL_MEMORY, mem,
                               PROC_COL_GPU, gpu_col,
                               -1);
            g_free(gpu_col);
            g_free(shown);
            g_free(mem);
            g_free(pid);
            g_free(name);
        }
        g_list_free(order);
        g_hash_table_destroy(table);
    }

    if (nqueried == 0 && nsuspended > 0) {
        msg = g_strdup("The NVIDIA GPU is suspended (runtime power "
                       "management), so no application is using it.");
    } else if (nqueried == 0) {
        msg = g_strdup("Unable to query the NVIDIA GPU through NVML.");
    } else if (napps == 0) {
        msg = g_strdup("No applications are currently offloaded to the "
                       "NVIDIA GPU.");
    } else {
        msg = g_strdup_printf("%d application%s using the NVIDIA GPU.",
                              napps, napps == 1 ? " is" : "s are");
    }
    gtk_label_set_text(GTK_LABEL(page->proc_status_label), msg);
    g_free(msg);
}


static gboolean update_live(gpointer data)
{
    CtkPrime *page = CTK_PRIME(data);

    update_power(page);
    update_processes(page);

    /*
     * Wayland: the hotplug event arrives while the compositor is still
     * switching outputs, so pick up the state it settles in.
     */
    if (page->wl_state) {
        gchar *now = dc_connector_state_signature();
        if (strcmp(now, page->wl_state) != 0) {
            ctk_prime_refresh(page);
        }
        g_free(now);
    }
    return TRUE;
}


/*
 * Wayland: there are no RandR providers.  The compositor drives every GPU's
 * outputs itself, so show which GPUs have displays (from KMS) and which
 * ones the compositor has open.
 */
static void refresh_wayland(CtkPrime *page)
{
    const DcCompositor *comp = dc_compositor();
    GList *cards, *l, *c;
    GString *display_gpus = g_string_new(NULL);
    GtkWidget *table;
    gboolean nv_has_display = FALSE;
    gint row = 0;
    gchar *s;

    /* Before reading the state, so a change while reading is noticed */
    g_free(page->wl_state);
    page->wl_state = dc_connector_state_signature();
    cards = enumerate_cards();

    s = g_strdup_printf("Render offload (Wayland, %s)",
                        comp && comp->desktop ? comp->desktop :
                        "unknown compositor");
    gtk_label_set_text(GTK_LABEL(page->mode_label), s);
    g_free(s);

    gtk_label_set_text(GTK_LABEL(page->offload_gpu_label),
                       ctk_prime_nvidia_gpu_label(page));
    gtk_label_set_text(GTK_LABEL(page->offload_label),
                       "Available.  The environment variables below offload "
                       "Vulkan, EGL and\nXWayland GLX applications.");

    table = new_table();
    gtk_table_resize(GTK_TABLE(table), 1, 3);
    grid_cell(table, 0, row, "GPU", TRUE);
    grid_cell(table, 1, row, "Driver", TRUE);
    grid_cell(table, 2, row, "Displays", TRUE);
    row++;

    for (l = cards; l; l = l->next) {
        DrmCard *card = l->data;
        GList *conns = dc_kms_connected(card->name);
        GString *outs = g_string_new(NULL);
        gboolean active = FALSE;
        gchar *gpu = card_label(page, card);

        for (c = conns; c; c = c->next) {
            DcConnector *dc = c->data;
            if (outs->len) g_string_append(outs, ", ");
            g_string_append(outs, dc->name);
            if (dc->monitor) {
                g_string_append_printf(outs, " (%s)", dc->monitor);
            }
            if (!dc->active) {
                g_string_append(outs, " (off)");
            } else {
                active = TRUE;
            }
        }
        if (active) {
            if (display_gpus->len) g_string_append(display_gpus, "\n");
            g_string_append(display_gpus, gpu);
            if (card->vendor == NVIDIA_PCI_VENDOR_ID) {
                nv_has_display = TRUE;
            }
        }

        gtk_table_resize(GTK_TABLE(table), row + 1, 3);
        grid_cell(table, 0, row, gpu, FALSE);
        grid_cell(table, 1, row, card->driver, FALSE);
        grid_cell(table, 2, row, outs->len ? outs->str : "None", FALSE);
        row++;

        g_string_free(outs, TRUE);
        g_free(gpu);
        dc_connector_list_free(conns);
    }

    gtk_label_set_text(GTK_LABEL(page->display_gpu_label),
                       display_gpus->len ? display_gpus->str : "Unknown");
    g_string_free(display_gpus, TRUE);

    gtk_box_pack_start(GTK_BOX(page->providers_box), table, FALSE, FALSE, 0);

    if (comp && comp->cards) {
        GString *used = g_string_new(NULL);
        GtkWidget *w;

        for (c = comp->cards; c; c = c->next) {
            DrmCard *card = NULL;
            for (l = cards; l; l = l->next) {
                if (strcmp(((DrmCard *) l->data)->name, c->data) == 0) {
                    card = l->data;
                }
            }
            if (card) {
                gchar *gpu = card_label(page, card);
                if (used->len) g_string_append(used, ", ");
                g_string_append(used, gpu);
                g_free(gpu);
            }
        }
        s = g_strdup_printf("The compositor (%s) has these GPUs open: %s.",
                            comp->process ? comp->process : "unknown",
                            used->len ? used->str : "none");
        w = left_label(s, FALSE);
        gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
        gtk_box_pack_start(GTK_BOX(page->providers_box), w, FALSE, FALSE, 5);
        g_free(s);
        g_string_free(used, TRUE);
    }
    if (nv_has_display) {
        GtkWidget *w = left_label("Displays connected to the NVIDIA GPU are "
                                  "driven directly by the compositor; reverse "
                                  "PRIME is not needed under Wayland.  They "
                                  "appear as pages under the NVIDIA GPU.",
                                  FALSE);
        gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
        gtk_box_pack_start(GTK_BOX(page->providers_box), w, FALSE, FALSE, 5);
    }

    gtk_widget_show_all(page->providers_box);
    g_list_free_full(cards, drm_card_free);
    update_env_labels(page);
}


void ctk_prime_refresh(CtkPrime *page)
{
    Window root;
    XRRScreenResources *res = NULL;
    XRRProviderResources *pr = NULL;
    GList *cards;
    GtkWidget *table, *otable = NULL;
    gint row = 0, orow = 0, i, j;
    gint num_nv_screens = 0;
    gchar *display_gpu = NULL, *nv_provider_name = NULL;
    gchar *display_provider_name = NULL;
    gboolean display_has_active = FALSE;
    gchar *s;

    page->nv_provider = None;
    page->display_provider = None;
    page->rprime_enabled = FALSE;
    page->rprime_active = FALSE;
    g_free(page->offload_provider);
    page->offload_provider = NULL;
    g_free(page->nv_provider_label);
    page->nv_provider_label = NULL;

    clear_container(page->providers_box);
    if (page->outputs_box) {
        clear_container(page->outputs_box);
    }

    if (!page->dpy && dc_session_type() == DC_SESSION_WAYLAND) {
        refresh_wayland(page);
        return;
    }

    if (!have_randr_providers(page->dpy)) {
        gtk_label_set_text(GTK_LABEL(page->mode_label),
                           page->dpy ? "Unknown (XRandR 1.4 is required)"
                                     : "Unknown (not running on X11)");
        gtk_label_set_text(GTK_LABEL(page->display_gpu_label), "Unknown");
        gtk_label_set_text(GTK_LABEL(page->offload_gpu_label),
                           page->nv_gpus ?
                           ((CtkPrimeGpu *) page->nv_gpus->data)->label :
                           "Unknown");
        gtk_label_set_text(GTK_LABEL(page->offload_label),
                           page->dpy ? "Unknown" :
                           "Managed by the Wayland compositor.\nThe "
                           "environment variables below offload Vulkan, EGL "
                           "and XWayland GLX applications.");
        gtk_box_pack_start(GTK_BOX(page->providers_box),
                           left_label("Provider information requires an X11 "
                                      "display with XRandR 1.4.", FALSE),
                           FALSE, FALSE, 0);
        gtk_widget_show_all(page->providers_box);
        if (page->outputs_section) {
            gtk_widget_set_sensitive(page->outputs_section, FALSE);
        }
        update_env_labels(page);
        return;
    }

    root = DefaultRootWindow(page->dpy);
    trap_push();
    res = XRRGetScreenResourcesCurrent(page->dpy, root);
    pr = res ? XRRGetProviderResources(page->dpy, root) : NULL;
    trap_pop();
    cards = enumerate_cards();

    table = new_table();
    gtk_table_resize(GTK_TABLE(table), 1, 4);
    grid_cell(table, 0, row, "Provider", TRUE);
    grid_cell(table, 1, row, "GPU", TRUE);
    grid_cell(table, 2, row, "Capabilities", TRUE);
    grid_cell(table, 3, row, "Displays", TRUE);
    row++;

    for (i = 0; pr && i < pr->nproviders; i++) {
        XRRProviderInfo *pi = XRRGetProviderInfo(page->dpy, res,
                                                 pr->providers[i]);
        DrmCard *card;
        gchar *gpu, *caps;
        GString *outs = g_string_new(NULL);
        gboolean active = FALSE;

        if (!pi) continue;
        card = provider_card(page->dpy, cards, pi);
        gpu = card_label(page, card);
        caps = caps_string(pi->capabilities);

        for (j = 0; j < pi->noutputs; j++) {
            XRROutputInfo *oi = XRRGetOutputInfo(page->dpy, res,
                                                 pi->outputs[j]);
            if (!oi) continue;
            if (oi->connection == RR_Connected) {
                if (outs->len) g_string_append(outs, ", ");
                g_string_append_len(outs, oi->name, oi->nameLen);
                if (oi->crtc == None) {
                    g_string_append(outs, " (off)");
                } else {
                    active = TRUE;
                }
            }
            XRRFreeOutputInfo(oi);
        }

        gtk_table_resize(GTK_TABLE(table), row + 1, 4);
        grid_cell(table, 0, row, pi->name, FALSE);
        grid_cell(table, 1, row, gpu, FALSE);
        grid_cell(table, 2, row, caps, FALSE);
        grid_cell(table, 3, row, outs->len ? outs->str : "None", FALSE);
        row++;

        if (is_nv_gpu_screen(pi)) {
            num_nv_screens++;
            if (page->nv_provider == None) {
                page->nv_provider = pr->providers[i];
                nv_provider_name = g_strdup(pi->name);
                page->nv_provider_label = g_strdup(gpu);
                page->rprime_enabled =
                    (pi->capabilities & RR_Capability_SinkOutput) &&
                    pi->nassociatedproviders > 0;
                page->rprime_active = active;

                /* NVIDIA outputs table */
                if (page->outputs_box) {
                    otable = new_table();
                    gtk_table_resize(GTK_TABLE(otable), 1, 4);
                    grid_cell(otable, 0, orow, "Output", TRUE);
                    grid_cell(otable, 1, orow, "Status", TRUE);
                    grid_cell(otable, 2, orow, "Mode", TRUE);
                    grid_cell(otable, 3, orow, "Monitor", TRUE);
                    orow++;
                    for (j = 0; j < pi->noutputs; j++) {
                        XRROutputInfo *oi =
                            XRRGetOutputInfo(page->dpy, res, pi->outputs[j]);
                        gchar *name, *mode = NULL, *mon = NULL;
                        if (!oi) continue;
                        name = g_strndup(oi->name, oi->nameLen);
                        if (oi->connection == RR_Connected) {
                            mon = output_monitor_name(page->dpy,
                                                      pi->outputs[j]);
                            if (oi->crtc != None) {
                                XRRCrtcInfo *ci =
                                    XRRGetCrtcInfo(page->dpy, res, oi->crtc);
                                if (ci) {
                                    mode = g_strdup_printf("%ux%u+%d+%d",
                                                           ci->width,
                                                           ci->height,
                                                           ci->x, ci->y);
                                    XRRFreeCrtcInfo(ci);
                                }
                            }
                        }
                        gtk_table_resize(GTK_TABLE(otable), orow + 1, 4);
                        grid_cell(otable, 0, orow, name, FALSE);
                        grid_cell(otable, 1, orow,
                                  oi->connection == RR_Connected ?
                                  "Connected" : "Disconnected", FALSE);
                        grid_cell(otable, 2, orow,
                                  mode ? mode :
                                  oi->connection == RR_Connected ? "Off" : "",
                                  FALSE);
                        grid_cell(otable, 3, orow, mon ? mon : "", FALSE);
                        orow++;
                        g_free(mon);
                        g_free(mode);
                        g_free(name);
                        XRRFreeOutputInfo(oi);
                    }
                }
            }
        } else if (!is_nvidia_provider(pi) &&
                   (pi->capabilities & RR_Capability_SourceOutput) &&
                   (page->display_provider == None ||
                    (active && !display_has_active))) {
            page->display_provider = pr->providers[i];
            display_has_active = active;
            g_free(display_gpu);
            display_gpu = g_strdup(gpu);
            g_free(display_provider_name);
            display_provider_name = g_strdup(pi->name);
        }

        g_string_free(outs, TRUE);
        g_free(caps);
        g_free(gpu);
        XRRFreeProviderInfo(pi);
    }

    gtk_box_pack_start(GTK_BOX(page->providers_box), table, FALSE, FALSE, 0);
    gtk_widget_show_all(page->providers_box);

    /* status */
    if (page->nv_provider != None && page->rprime_active) {
        s = g_strdup("Render offload and reverse PRIME");
    } else if (page->nv_provider != None) {
        s = g_strdup("Render offload");
    } else if (page->have_nv_screen) {
        s = g_strdup("NVIDIA X screen");
    } else {
        s = g_strdup("Not configured");
    }
    gtk_label_set_text(GTK_LABEL(page->mode_label), s);
    g_free(s);

    if (display_gpu) {
        s = g_strdup_printf("%s (RandR provider %s)", display_gpu,
                            display_provider_name);
    } else {
        s = g_strdup("Unknown");
    }
    gtk_label_set_text(GTK_LABEL(page->display_gpu_label), s);
    g_free(s);

    if (page->nv_provider != None) {
        s = g_strdup_printf("%s (RandR provider %s)", page->nv_provider_label,
                            nv_provider_name);
    } else {
        s = g_strdup(page->nv_gpus ?
                     ((CtkPrimeGpu *) page->nv_gpus->data)->label : "Unknown");
    }
    gtk_label_set_text(GTK_LABEL(page->offload_gpu_label), s);
    g_free(s);

    if (page->nv_provider != None) {
        gtk_label_set_text(GTK_LABEL(page->offload_label), "Available");
    } else {
        gtk_label_set_text(GTK_LABEL(page->offload_label),
                           "Unavailable: the X server did not create an "
                           "NVIDIA GPU screen.\nAdd Option "
                           "\"AllowNVIDIAGPUScreens\" to the ServerLayout "
                           "section of xorg.conf\nand restart X.");
    }

    if (num_nv_screens > 1 && nv_provider_name) {
        page->offload_provider = g_strdup(nv_provider_name);
    }
    update_env_labels(page);

    /* reverse PRIME */
    if (page->outputs_section) {
        gtk_widget_set_sensitive(page->outputs_section,
                                 page->nv_provider != None);
        if (page->nv_provider == None) {
            gtk_label_set_text(GTK_LABEL(page->rprime_label),
                               "Unavailable without an NVIDIA GPU screen.");
            gtk_widget_hide(page->rprime_button);
        } else if (page->rprime_enabled) {
            s = g_strdup_printf("Enabled (output source: %s)",
                                display_provider_name ?
                                display_provider_name : "display GPU");
            gtk_label_set_text(GTK_LABEL(page->rprime_label), s);
            g_free(s);
            gtk_button_set_label(GTK_BUTTON(page->rprime_button),
                                 "Disable Reverse PRIME");
            gtk_widget_show(page->rprime_button);
        } else {
            gtk_label_set_text(GTK_LABEL(page->rprime_label), "Disabled");
            gtk_button_set_label(GTK_BUTTON(page->rprime_button),
                                 "Enable Reverse PRIME");
            gtk_widget_show(page->rprime_button);
        }
        if (otable) {
            gtk_box_pack_start(GTK_BOX(page->outputs_box), otable,
                               FALSE, FALSE, 0);
            gtk_widget_show_all(page->outputs_box);
        }
    }

    g_free(display_gpu);
    g_free(display_provider_name);
    g_free(nv_provider_name);
    g_list_free_full(cards, drm_card_free);
    if (pr) XRRFreeProviderResources(pr);
    if (res) XRRFreeScreenResources(res);
}


const gchar *ctk_prime_nvidia_gpu_label(CtkPrime *page)
{
    if (page->nv_provider_label) {
        return page->nv_provider_label;
    }
    if (page->nv_gpus) {
        return ((CtkPrimeGpu *) page->nv_gpus->data)->label;
    }
    return "NVIDIA GPU";
}


/* ---- construction ------------------------------------------------------- */

static void page_destroyed(GObject *obj, gpointer data)
{
    CtkPrime *page = CTK_PRIME(obj);

    g_list_free_full(page->nv_gpus, ctk_prime_gpu_free);
    g_list_free_full(page->card_names, ctk_prime_card_name_free);
    page->nv_gpus = page->card_names = NULL;
    g_free(page->offload_provider);
    g_free(page->nv_provider_label);
    page->offload_provider = page->nv_provider_label = NULL;
    g_free(page->wl_state);
    page->wl_state = NULL;
    if (page->proc_store) {
        g_object_unref(page->proc_store);
        page->proc_store = NULL;
    }
}


static gboolean nvidia_gpu_screen_exists(Display *dpy)
{
    Window root;
    XRRScreenResources *res;
    XRRProviderResources *pr;
    gboolean found = FALSE;
    int i;

    if (!have_randr_providers(dpy)) {
        return FALSE;
    }
    root = DefaultRootWindow(dpy);
    res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return FALSE;
    pr = XRRGetProviderResources(dpy, root);
    for (i = 0; pr && i < pr->nproviders && !found; i++) {
        XRRProviderInfo *pi = XRRGetProviderInfo(dpy, res, pr->providers[i]);
        if (pi) {
            found = is_nv_gpu_screen(pi);
            XRRFreeProviderInfo(pi);
        }
    }
    if (pr) XRRFreeProviderResources(pr);
    XRRFreeScreenResources(res);
    return found;
}


static void add_text_column(GtkWidget *view, const gchar *title, gint col,
                            gboolean expand)
{
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c =
        gtk_tree_view_column_new_with_attributes(title, r, "text", col, NULL);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_expand(c, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
}


static GtkWidget *selectable_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    return l;
}


GtkWidget *ctk_prime_new(Display *dpy, GList *nv_gpus, GList *card_names,
                         gboolean have_nv_screen, CtkConfig *ctk_config)
{
    GObject *object;
    CtkPrime *page;
    GtkWidget *banner, *vbox, *sw, *sec, *table, *w, *hbox, *button, *view;
    gchar *layer;
    gint row;

    /*
     * The page applies to the usual hybrid configuration with no NVIDIA X
     * screen, and to any configuration with an NVIDIA GPU screen.
     */
    if (!nv_gpus || (have_nv_screen && !nvidia_gpu_screen_exists(dpy))) {
        g_list_free_full(nv_gpus, ctk_prime_gpu_free);
        g_list_free_full(card_names, ctk_prime_card_name_free);
        return NULL;
    }

    object = g_object_new(CTK_TYPE_PRIME, NULL);
    page = CTK_PRIME(object);
    page->ctk_config = ctk_config;
    page->dpy = dpy;
    page->have_nv_screen = have_nv_screen;
    page->nv_gpus = nv_gpus;
    page->card_names = card_names;
    g_signal_connect(object, "destroy", G_CALLBACK(page_destroyed), NULL);

    gtk_box_set_spacing(GTK_BOX(page), 5);
    banner = ctk_banner_image_new(BANNER_ARTWORK_BLANK);
    gtk_box_pack_start(GTK_BOX(page), banner, FALSE, FALSE, 0);

    vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 5);
    sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw),
                                        GTK_SHADOW_NONE);
    ctk_scrolled_window_add(GTK_SCROLLED_WINDOW(sw), vbox);
    gtk_box_pack_start(GTK_BOX(page), sw, TRUE, TRUE, 0);

    /* --- status --- */
    sec = ctk_section_box_new(vbox, "PRIME Configuration");
    table = new_table();
    gtk_box_pack_start(GTK_BOX(sec), table, FALSE, FALSE, 0);
    row = 0;
    page->mode_label = info_row(page, table, &row, "PRIME Mode:", __mode_help);
    page->display_gpu_label = info_row(page, table, &row, "Display GPU:",
                                       __mode_help);
    page->offload_gpu_label = info_row(page, table, &row,
                                       "Render Offload GPU:", __mode_help);
    page->offload_label = info_row(page, table, &row, "Render Offload:",
                                   __offload_help);
    page->vulkan_label = info_row(page, table, &row, "Vulkan Offload Layer:",
                                  __vulkan_help);
    page->power_label = info_row(page, table, &row, "NVIDIA GPU Power State:",
                                 __power_help);

    layer = find_vulkan_layer();
    if (layer) {
        gchar *s = g_strdup_printf("Installed (%s)", layer);
        gtk_label_set_text(GTK_LABEL(page->vulkan_label), s);
        g_free(s);
    } else {
        gtk_label_set_text(GTK_LABEL(page->vulkan_label),
                           "Not found: Vulkan applications cannot be "
                           "offloaded\n(VK_LAYER_NV_optimus is missing).");
    }
    g_free(layer);

    sec = ctk_section_box_new(vbox, (!dpy && dc_session_type() == DC_SESSION_WAYLAND)
                            ? "GPU Displays" : "XRandR Providers");
    page->providers_box = gtk_vbox_new(FALSE, 0);
    ctk_config_set_tooltip(ctk_config, page->providers_box, __providers_help);
    gtk_box_pack_start(GTK_BOX(sec), page->providers_box, FALSE, FALSE, 0);

    /* --- applications --- */
    sec = ctk_section_box_new(vbox, "Applications Using the NVIDIA GPU");
    page->proc_store = gtk_list_store_new(PROC_NUM_COLS, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_STRING);
    view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(page->proc_store));
    add_text_column(view, "PID", PROC_COL_PID, FALSE);
    add_text_column(view, "Application", PROC_COL_NAME, TRUE);
    add_text_column(view, "Type", PROC_COL_TYPE, FALSE);
    add_text_column(view, "GPU Memory", PROC_COL_MEMORY, FALSE);
    if (g_list_length(nv_gpus) > 1) {
        add_text_column(view, "GPU", PROC_COL_GPU, FALSE);
    }
    ctk_config_set_tooltip(ctk_config, view, __procs_help);
    w = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(w),
                                        GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(w), view);
    gtk_widget_set_size_request(w, -1, 140);
    gtk_box_pack_start(GTK_BOX(sec), w, FALSE, FALSE, 0);
    page->proc_status_label = left_label("", FALSE);
    gtk_box_pack_start(GTK_BOX(sec), page->proc_status_label, FALSE, FALSE, 0);

    /* --- launcher --- */
    sec = ctk_section_box_new(vbox, "Run an Application on the NVIDIA GPU");
    hbox = gtk_hbox_new(FALSE, 5);
    gtk_box_pack_start(GTK_BOX(hbox), left_label("Command:", FALSE),
                       FALSE, FALSE, 0);
    page->cmd_entry = gtk_entry_new();
    gtk_box_pack_start(GTK_BOX(hbox), page->cmd_entry, TRUE, TRUE, 0);
    button = gtk_button_new_with_label("Run on NVIDIA GPU");
    gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
    ctk_config_set_tooltip(ctk_config, page->cmd_entry, __launch_help);
    ctk_config_set_tooltip(ctk_config, button, __launch_help);
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(run_clicked),
                     page);
    g_signal_connect(G_OBJECT(page->cmd_entry), "activate",
                     G_CALLBACK(run_clicked), page);
    gtk_box_pack_start(GTK_BOX(sec), hbox, FALSE, FALSE, 0);

    page->vk_only_check =
        gtk_check_button_new_with_label("Vulkan: expose only the NVIDIA GPU");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->vk_only_check), TRUE);
    ctk_config_set_tooltip(ctk_config, page->vk_only_check, __vk_only_help);
    g_signal_connect(G_OBJECT(page->vk_only_check), "toggled",
                     G_CALLBACK(vk_only_toggled), page);
    gtk_box_pack_start(GTK_BOX(sec), page->vk_only_check, FALSE, FALSE, 0);

    table = new_table();
    gtk_table_resize(GTK_TABLE(table), 2, 2);
    gtk_table_attach(GTK_TABLE(table), left_label("Environment:", FALSE),
                     0, 1, 0, 1, GTK_FILL, GTK_FILL, 0, 0);
    page->env_label = selectable_label();
    gtk_table_attach(GTK_TABLE(table), page->env_label, 1, 2, 0, 1,
                     GTK_FILL | GTK_EXPAND, GTK_FILL, 0, 0);
    gtk_table_attach(GTK_TABLE(table),
                     left_label("Steam Launch Options:", FALSE),
                     0, 1, 1, 2, GTK_FILL, GTK_FILL, 0, 0);
    page->steam_label = selectable_label();
    gtk_table_attach(GTK_TABLE(table), page->steam_label, 1, 2, 1, 2,
                     GTK_FILL | GTK_EXPAND, GTK_FILL, 0, 0);
    gtk_box_pack_start(GTK_BOX(sec), table, FALSE, FALSE, 0);

    /* --- reverse PRIME --- */
    if (dpy) {
        page->outputs_section =
            ctk_section_box_new(vbox, "NVIDIA GPU Display Outputs (Reverse PRIME)");
        sec = page->outputs_section;
        hbox = gtk_hbox_new(FALSE, 10);
        gtk_box_pack_start(GTK_BOX(hbox), left_label("Reverse PRIME:", FALSE),
                           FALSE, FALSE, 0);
        page->rprime_label = left_label("", FALSE);
        gtk_box_pack_start(GTK_BOX(hbox), page->rprime_label, FALSE, FALSE, 0);
        page->rprime_button = gtk_button_new_with_label("Enable Reverse PRIME");
        ctk_config_set_tooltip(ctk_config, page->rprime_button, __rprime_help);
        g_signal_connect(G_OBJECT(page->rprime_button), "clicked",
                         G_CALLBACK(rprime_clicked), page);
        gtk_box_pack_end(GTK_BOX(hbox), page->rprime_button, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sec), hbox, FALSE, FALSE, 0);

        page->outputs_box = gtk_vbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sec), page->outputs_box, FALSE, FALSE, 0);
        w = left_label("Displays connected to the NVIDIA GPU appear as pages "
                       "below this one, where they can be enabled and "
                       "configured.", FALSE);
        gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
        gtk_box_pack_start(GTK_BOX(sec), w, FALSE, FALSE, 0);
    }

    ctk_config_add_timer(ctk_config, PRIME_UPDATE_INTERVAL_MS,
                         "PRIME Render Offload Monitor",
                         (GSourceFunc) update_live, (gpointer) page);

    gtk_widget_show_all(GTK_WIDGET(page));
    ctk_prime_refresh(page);
    update_live(page);

    return GTK_WIDGET(page);
}


void ctk_prime_start_timer(GtkWidget *widget)
{
    CtkPrime *page = CTK_PRIME(widget);
    update_live(page);
    ctk_config_start_timer(page->ctk_config, (GSourceFunc) update_live,
                           (gpointer) page);
}


void ctk_prime_stop_timer(GtkWidget *widget)
{
    CtkPrime *page = CTK_PRIME(widget);
    ctk_config_stop_timer(page->ctk_config, (GSourceFunc) update_live,
                          (gpointer) page);
}


GtkTextBuffer *ctk_prime_create_help(GtkTextTagTable *table, CtkPrime *page)
{
    GtkTextIter i;
    GtkTextBuffer *b = gtk_text_buffer_new(table);

    gtk_text_buffer_get_iter_at_offset(b, &i, 0);

    ctk_help_title(b, &i, "PRIME Help");
    ctk_help_para(b, &i, "This page describes how the NVIDIA GPU works "
                  "together with the GPU that drives the X screen (for "
                  "example an integrated AMD or Intel GPU).  In this "
                  "configuration there is no NVIDIA X screen, so the "
                  "information is gathered from XRandR, NVML and sysfs.");

    ctk_help_heading(b, &i, "PRIME Mode");
    ctk_help_para(b, &i, "%s", __mode_help);
    ctk_help_heading(b, &i, "Render Offload");
    ctk_help_para(b, &i, "%s", __offload_help);
    ctk_help_heading(b, &i, "Vulkan Offload Layer");
    ctk_help_para(b, &i, "%s", __vulkan_help);
    ctk_help_heading(b, &i, "NVIDIA GPU Power State");
    ctk_help_para(b, &i, "%s", __power_help);
    ctk_help_heading(b, &i, "XRandR Providers");
    ctk_help_para(b, &i, "%s", __providers_help);
    ctk_help_heading(b, &i, "Applications Using the NVIDIA GPU");
    ctk_help_para(b, &i, "%s", __procs_help);
    ctk_help_heading(b, &i, "Run an Application on the NVIDIA GPU");
    ctk_help_para(b, &i, "%s", __launch_help);
    ctk_help_para(b, &i, "__NV_PRIME_RENDER_OFFLOAD=1 enables offloading, "
                  "__GLX_VENDOR_LIBRARY_NAME=nvidia selects the NVIDIA GLX "
                  "implementation, and __NV_PRIME_RENDER_OFFLOAD_PROVIDER "
                  "chooses the NVIDIA GPU when there are several.  Copy the "
                  "'Steam Launch Options' text into a game's launch options "
                  "to always run it on the NVIDIA GPU.");
    ctk_help_heading(b, &i, "Vulkan: expose only the NVIDIA GPU");
    ctk_help_para(b, &i, "%s", __vk_only_help);
    if (page->outputs_section) {
        ctk_help_heading(b, &i, "Reverse PRIME");
        ctk_help_para(b, &i, "%s", __rprime_help);
    }

    ctk_help_finish(b);
    return b;
}
