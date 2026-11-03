/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * PRIME render offload / reverse PRIME page, for systems where another GPU
 * drives the X screen and the NVIDIA GPU renders offloaded applications.
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

#ifndef __CTK_PRIME_H__
#define __CTK_PRIME_H__

#include <gtk/gtk.h>
#include <X11/Xlib.h>

#include "ctkconfig.h"

G_BEGIN_DECLS

#define CTK_TYPE_PRIME (ctk_prime_get_type())

#define CTK_PRIME(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST ((obj), CTK_TYPE_PRIME, CtkPrime))

#define CTK_IS_PRIME(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE ((obj), CTK_TYPE_PRIME))

typedef struct _CtkPrime       CtkPrime;
typedef struct _CtkPrimeClass  CtkPrimeClass;

/* An NVIDIA GPU known to NV-CONTROL / NVML */
typedef struct _CtkPrimeGpu {
    gchar *pci_slot;     /* "0000:01:00.0", may be NULL */
    gchar *card;         /* "card0", may be NULL */
    gchar *label;        /* "card0 - (NVIDIA GeForce RTX 5070)" */
} CtkPrimeGpu;

/* A GPU label hint for non-NVIDIA DRM cards (e.g. AMD product names) */
typedef struct _CtkPrimeCardName {
    gchar *card;         /* "card1" */
    gchar *label;        /* "card1 - (AMD Radeon 610M)" */
} CtkPrimeCardName;

struct _CtkPrime
{
    GtkVBox parent;

    CtkConfig *ctk_config;
    Display *dpy;                /* NULL when not running on X11 */
    gboolean have_nv_screen;

    GList *nv_gpus;              /* CtkPrimeGpu*, owned */
    GList *card_names;           /* CtkPrimeCardName*, owned */

    /* status */
    GtkWidget *mode_label;
    GtkWidget *display_gpu_label;
    GtkWidget *offload_gpu_label;
    GtkWidget *offload_label;
    GtkWidget *vulkan_label;
    GtkWidget *power_label;
    GtkWidget *providers_box;

    /* applications */
    GtkListStore *proc_store;
    GtkWidget *proc_status_label;

    /* launcher */
    GtkWidget *cmd_entry;
    GtkWidget *vk_only_check;
    GtkWidget *env_label;
    GtkWidget *steam_label;
    gchar *offload_provider;     /* set when several NVIDIA GPU screens */

    /* reverse PRIME */
    GtkWidget *outputs_section;
    GtkWidget *rprime_label;
    GtkWidget *rprime_button;
    GtkWidget *outputs_box;
    XID nv_provider;             /* RRProvider of the NVIDIA GPU screen */
    XID display_provider;        /* RRProvider driving the X screen */
    gboolean rprime_enabled;
    gboolean rprime_active;      /* an NVIDIA output is lit */
    gchar *nv_provider_label;    /* GPU label of nv_provider */
};

struct _CtkPrimeClass
{
    GtkVBoxClass parent_class;
};

GType          ctk_prime_get_type    (void) G_GNUC_CONST;

/*
 * Returns a PRIME page, or NULL if it does not apply (no NVIDIA GPU, or an
 * NVIDIA X screen without NVIDIA GPU screens).  Takes ownership of 'nv_gpus'
 * (CtkPrimeGpu*) and 'card_names' (CtkPrimeCardName*) in every case.
 */
GtkWidget*     ctk_prime_new         (Display *dpy, GList *nv_gpus,
                                      GList *card_names,
                                      gboolean have_nv_screen,
                                      CtkConfig *ctk_config);
GtkTextBuffer* ctk_prime_create_help (GtkTextTagTable *, CtkPrime *);

void           ctk_prime_start_timer (GtkWidget *);
void           ctk_prime_stop_timer  (GtkWidget *);

/* Re-read the RandR provider/output state */
void           ctk_prime_refresh     (CtkPrime *);

/* Label for the NVIDIA GPU that owns the NVIDIA GPU screen outputs */
const gchar   *ctk_prime_nvidia_gpu_label(CtkPrime *);

/* Returns the connected outputs (RROutput as GUINT_TO_POINTER) of NVIDIA
 * GPU screen providers ("NVIDIA-G0", ...). */
GList         *ctk_prime_find_nvidia_outputs(Display *dpy);

void           ctk_prime_gpu_free    (gpointer gpu);
void           ctk_prime_card_name_free(gpointer name);

G_END_DECLS

#endif /* __CTK_PRIME_H__ */
