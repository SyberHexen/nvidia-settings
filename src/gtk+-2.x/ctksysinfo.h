/*
 * nvidia-settings: A tool for configuring the NVIDIA X driver on Unix
 * and Linux systems.
 *
 * Extra System Information sections: Vulkan devices and AMD driver
 * information.
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

#ifndef __CTK_SYSINFO_H__
#define __CTK_SYSINFO_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

/*
 * Append the "Vulkan Information", "OpenGL Information" and (when an
 * amdgpu GPU is present) "AMD Driver Information" sections to the System
 * Information page.  Sets each *have_* flag to whether its section was
 * added.
 */
void ctk_sysinfo_add_sections(GtkWidget *vbox, gboolean *have_vulkan,
                              gboolean *have_gl, gboolean *have_amd);

void ctk_sysinfo_add_help(GtkTextBuffer *b, GtkTextIter *i,
                          gboolean have_vulkan, gboolean have_gl,
                          gboolean have_amd);

G_END_DECLS

#endif /* __CTK_SYSINFO_H__ */
