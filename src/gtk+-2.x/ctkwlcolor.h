/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * Color correction (brightness, contrast, gamma) of a display under
 * Wayland, applied as gamma ramps through the compositor.
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

#ifndef __CTK_WL_COLOR_H__
#define __CTK_WL_COLOR_H__

#include <gtk/gtk.h>

#include "ctkconfig.h"
#include "displaycfg.h"

G_BEGIN_DECLS

/* The color correction controls for the compositor output 'output', or a
 * label explaining why there are none.  'backend' must support gamma. */
GtkWidget *ctk_wl_color_correction_new(DcBackend *backend,
                                       const gchar *output,
                                       CtkConfig *ctk_config);

void ctk_wl_color_correction_help(GtkTextBuffer *b, GtkTextIter *i);

G_END_DECLS

#endif /* __CTK_WL_COLOR_H__ */
