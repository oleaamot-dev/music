/* gj-project.h - GarageJam project model and GJXML serialisation
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GJ_PROJECT_H
#define GJ_PROJECT_H

#include <glib.h>

G_BEGIN_DECLS

#define GJ_ERROR (gj_error_quark ())
GQuark gj_error_quark (void);

typedef enum {
  GJ_ERROR_PARSE,
  GJ_ERROR_PLUGIN,
  GJ_ERROR_NOTHING_TO_PLAY,
  GJ_ERROR_BUSY,
  GJ_ERROR_STREAM
} GjErrorCode;

typedef struct {
  char    *name;
  char    *path;     /* absolute path to the audio file, or NULL if empty */
  double   volume;   /* linear gain, 0.0 – 2.0 */
  double   pan;      /* -1.0 (left) – 1.0 (right) */
  gboolean mute;
  gboolean solo;
  gboolean armed;    /* record-armed; not saved */
} GjTrack;

typedef struct {
  char      *title;
  char      *dir;       /* directory where takes are recorded */
  char      *filename;  /* .gjxml path, or NULL when never saved */
  int        rate;
  int        channels;
  double     bpm;
  GPtrArray *tracks;    /* element-type GjTrack, owned */
} GjProject;

GjTrack   *gj_track_new            (const char *name);
void       gj_track_free           (GjTrack *track);

GjProject *gj_project_new          (void);
void       gj_project_free         (GjProject *project);
GjTrack   *gj_project_add_track    (GjProject *project, const char *name);
void       gj_project_remove_track (GjProject *project, GjTrack *track);
gboolean   gj_project_any_solo     (GjProject *project);
gboolean   gj_project_track_audible(GjProject *project, GjTrack *track);
char      *gj_project_new_take_path(GjProject *project, GjTrack *track);
gboolean   gj_project_ensure_dir   (GjProject *project, GError **error);

char      *gj_project_to_xml       (GjProject *project, const char *base_dir);
GjProject *gj_project_from_xml     (const char *xml, gssize len,
                                    const char *base_dir, GError **error);
gboolean   gj_project_save         (GjProject *project, const char *path,
                                    GError **error);
GjProject *gj_project_load         (const char *path, GError **error);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GjProject, gj_project_free)

G_END_DECLS

#endif /* GJ_PROJECT_H */
