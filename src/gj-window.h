/* gj-window.h - GarageJam main window
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GJ_WINDOW_H
#define GJ_WINDOW_H

#include <adwaita.h>

G_BEGIN_DECLS

#define GJ_TYPE_WINDOW (gj_window_get_type ())
G_DECLARE_FINAL_TYPE (GjWindow, gj_window, GJ, WINDOW, AdwApplicationWindow)

GjWindow *gj_window_new       (AdwApplication *app);
gboolean  gj_window_open_file (GjWindow *self, GFile *file);

G_END_DECLS

#endif /* GJ_WINDOW_H */
