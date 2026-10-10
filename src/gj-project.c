/* gj-project.c - GarageJam project model and GJXML serialisation
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>

#include "gj-project.h"

G_DEFINE_QUARK (garagejam-error-quark, gj_error)

#define GJXML_VERSION "8.0.1"

GjTrack *
gj_track_new (const char *name)
{
  GjTrack *track = g_new0 (GjTrack, 1);

  track->name = g_strdup (name ? name : "Track");
  track->volume = 1.0;
  track->pan = 0.0;
  return track;
}

void
gj_track_free (GjTrack *track)
{
  if (track == NULL)
    return;
  g_free (track->name);
  g_free (track->path);
  g_free (track);
}

GjProject *
gj_project_new (void)
{
  GjProject *project = g_new0 (GjProject, 1);

  project->title = g_strdup ("Untitled Jam");
  project->rate = 48000;
  project->channels = 2;
  project->bpm = 120.0;
  project->tracks = g_ptr_array_new_with_free_func ((GDestroyNotify) gj_track_free);
  return project;
}

void
gj_project_free (GjProject *project)
{
  if (project == NULL)
    return;
  g_free (project->title);
  g_free (project->dir);
  g_free (project->filename);
  g_ptr_array_unref (project->tracks);
  g_free (project);
}

GjTrack *
gj_project_add_track (GjProject *project, const char *name)
{
  g_autofree char *auto_name = NULL;
  GjTrack *track;

  if (name == NULL)
    {
      auto_name = g_strdup_printf ("Track %u", project->tracks->len + 1);
      name = auto_name;
    }
  track = gj_track_new (name);
  g_ptr_array_add (project->tracks, track);
  return track;
}

void
gj_project_remove_track (GjProject *project, GjTrack *track)
{
  g_ptr_array_remove (project->tracks, track);
}

gboolean
gj_project_any_solo (GjProject *project)
{
  for (guint i = 0; i < project->tracks->len; i++)
    {
      GjTrack *t = g_ptr_array_index (project->tracks, i);
      if (t->solo)
        return TRUE;
    }
  return FALSE;
}

gboolean
gj_project_track_audible (GjProject *project, GjTrack *track)
{
  if (track->path == NULL || !g_file_test (track->path, G_FILE_TEST_IS_REGULAR))
    return FALSE;
  if (track->mute)
    return FALSE;
  if (gj_project_any_solo (project) && !track->solo)
    return FALSE;
  return TRUE;
}

gboolean
gj_project_ensure_dir (GjProject *project, GError **error)
{
  if (project->dir == NULL)
    {
      const char *music = g_get_user_special_dir (G_USER_DIRECTORY_MUSIC);
      g_autoptr (GDateTime) now = g_date_time_new_now_local ();
      g_autofree char *stamp = g_date_time_format (now, "Jam-%Y%m%d-%H%M%S");

      if (music == NULL)
        music = g_get_home_dir ();
      project->dir = g_build_filename (music, "GarageJam", stamp, NULL);
    }

  if (g_mkdir_with_parents (project->dir, 0755) != 0)
    {
      int saved = errno;
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (saved),
                   "Could not create project folder %s: %s",
                   project->dir, g_strerror (saved));
      return FALSE;
    }
  return TRUE;
}

static char *
slugify (const char *name)
{
  GString *s = g_string_new (NULL);

  for (const char *p = name; *p; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      if (g_unichar_isalnum (c))
        g_string_append_unichar (s, g_unichar_tolower (c));
      else if (s->len > 0 && s->str[s->len - 1] != '-')
        g_string_append_c (s, '-');
    }
  while (s->len > 0 && s->str[s->len - 1] == '-')
    g_string_truncate (s, s->len - 1);
  if (s->len == 0)
    g_string_append (s, "track");
  return g_string_free (s, FALSE);
}

char *
gj_project_new_take_path (GjProject *project, GjTrack *track)
{
  g_autofree char *slug = slugify (track->name);

  for (guint take = 1; take < 10000; take++)
    {
      g_autofree char *base = g_strdup_printf ("%s-take%u.wav", slug, take);
      char *path = g_build_filename (project->dir, base, NULL);
      if (!g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
      g_free (path);
    }
  return NULL;
}

/* ---------------------------------------------------------------------
 * GJXML writer
 * ------------------------------------------------------------------- */

static char *
relative_path (const char *path, const char *base_dir)
{
  if (base_dir != NULL)
    {
      g_autofree char *prefix = g_build_filename (base_dir, "/", NULL);
      if (g_str_has_prefix (path, prefix))
        return g_strdup (path + strlen (prefix));
    }
  return g_strdup (path);
}

static const char *
dtostr (char buf[G_ASCII_DTOSTR_BUF_SIZE], double v)
{
  /* locale-independent: Norwegian locales use ',' as decimal mark */
  return g_ascii_formatd (buf, G_ASCII_DTOSTR_BUF_SIZE, "%.3f", v);
}

char *
gj_project_to_xml (GjProject *project, const char *base_dir)
{
  GString *xml = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  char b1[G_ASCII_DTOSTR_BUF_SIZE], b2[G_ASCII_DTOSTR_BUF_SIZE];
  g_autofree char *head = NULL;

  g_string_append (xml, "<garagejam version=\"" GJXML_VERSION "\">\n");
  head = g_markup_printf_escaped ("  <project title=\"%s\" rate=\"%d\" "
                                  "channels=\"%d\" bpm=\"%s\">\n",
                                  project->title, project->rate,
                                  project->channels, dtostr (b1, project->bpm));
  g_string_append (xml, head);

  for (guint i = 0; i < project->tracks->len; i++)
    {
      GjTrack *t = g_ptr_array_index (project->tracks, i);
      g_autofree char *file = t->path ? relative_path (t->path, base_dir) : g_strdup ("");
      g_autofree char *line =
        g_markup_printf_escaped ("    <track name=\"%s\" file=\"%s\" "
                                 "volume=\"%s\" pan=\"%s\" mute=\"%s\" "
                                 "solo=\"%s\"/>\n",
                                 t->name, file,
                                 dtostr (b1, t->volume), dtostr (b2, t->pan),
                                 t->mute ? "true" : "false",
                                 t->solo ? "true" : "false");
      g_string_append (xml, line);
    }

  g_string_append (xml, "  </project>\n</garagejam>\n");
  return g_string_free (xml, FALSE);
}

/* ---------------------------------------------------------------------
 * GJXML parser
 * ------------------------------------------------------------------- */

typedef struct {
  GjProject  *project;
  const char *base_dir;
  gboolean    seen_root;
  gboolean    seen_project;
} ParseState;

static gboolean
parse_bool (const char *s)
{
  return s != NULL && (g_ascii_strcasecmp (s, "true") == 0 || strcmp (s, "1") == 0);
}

static double
parse_double (const char *s, double fallback, double min, double max)
{
  char *end = NULL;
  double v;

  if (s == NULL || *s == '\0')
    return fallback;
  v = g_ascii_strtod (s, &end);
  if (end == s)
    return fallback;
  return CLAMP (v, min, max);
}

static void
start_element (GMarkupParseContext *ctx,
               const char          *element,
               const char         **names,
               const char         **values,
               gpointer             user_data,
               GError             **error)
{
  ParseState *st = user_data;
  GjProject *project = st->project;

  if (strcmp (element, "garagejam") == 0)
    {
      st->seen_root = TRUE;
      return;
    }

  if (!st->seen_root)
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_PARSE,
                   "Not a GJXML document (root element is <%s>)", element);
      return;
    }

  if (strcmp (element, "project") == 0)
    {
      st->seen_project = TRUE;
      for (int i = 0; names[i]; i++)
        {
          if (strcmp (names[i], "title") == 0)
            {
              g_free (project->title);
              project->title = g_strdup (values[i]);
            }
          else if (strcmp (names[i], "rate") == 0)
            project->rate = (int) parse_double (values[i], 48000, 8000, 192000);
          else if (strcmp (names[i], "channels") == 0)
            project->channels = (int) parse_double (values[i], 2, 1, 2);
          else if (strcmp (names[i], "bpm") == 0)
            project->bpm = parse_double (values[i], 120, 20, 400);
        }
    }
  else if (strcmp (element, "track") == 0)
    {
      GjTrack *t = gj_track_new ("Track");

      for (int i = 0; names[i]; i++)
        {
          const char *v = values[i];
          if (strcmp (names[i], "name") == 0)
            {
              g_free (t->name);
              t->name = g_strdup (v);
            }
          else if (strcmp (names[i], "file") == 0 && *v)
            {
              if (g_path_is_absolute (v) || st->base_dir == NULL)
                t->path = g_strdup (v);
              else
                t->path = g_build_filename (st->base_dir, v, NULL);
            }
          else if (strcmp (names[i], "volume") == 0)
            t->volume = parse_double (v, 1.0, 0.0, 2.0);
          else if (strcmp (names[i], "pan") == 0)
            t->pan = parse_double (v, 0.0, -1.0, 1.0);
          else if (strcmp (names[i], "mute") == 0)
            t->mute = parse_bool (v);
          else if (strcmp (names[i], "solo") == 0)
            t->solo = parse_bool (v);
        }
      g_ptr_array_add (project->tracks, t);
    }
  /* Unknown elements are ignored for forward compatibility. */
}

GjProject *
gj_project_from_xml (const char *xml, gssize len, const char *base_dir,
                     GError **error)
{
  static const GMarkupParser parser = { start_element, NULL, NULL, NULL, NULL };
  g_autoptr (GjProject) project = gj_project_new ();
  ParseState st = { project, base_dir, FALSE, FALSE };
  GMarkupParseContext *ctx;
  gboolean ok;

  ctx = g_markup_parse_context_new (&parser, 0, &st, NULL);
  ok = g_markup_parse_context_parse (ctx, xml, len, error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_markup_parse_context_free (ctx);

  if (!ok)
    return NULL;
  if (!st.seen_root || !st.seen_project)
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_PARSE,
                   "Document has no <garagejam><project> element");
      return NULL;
    }
  return g_steal_pointer (&project);
}

gboolean
gj_project_save (GjProject *project, const char *path, GError **error)
{
  g_autofree char *filename = g_canonicalize_filename (path, NULL);
  g_autofree char *dir = g_path_get_dirname (filename);
  g_autofree char *xml = gj_project_to_xml (project, dir);

  if (!g_file_set_contents (filename, xml, -1, error))
    return FALSE;

  g_free (project->filename);
  project->filename = g_steal_pointer (&filename);
  /* New takes are recorded next to the saved project from now on. */
  g_free (project->dir);
  project->dir = g_steal_pointer (&dir);
  return TRUE;
}

GjProject *
gj_project_load (const char *path, GError **error)
{
  g_autofree char *filename = g_canonicalize_filename (path, NULL);
  g_autofree char *contents = NULL;
  gsize len = 0;
  g_autofree char *dir = g_path_get_dirname (filename);
  GjProject *project;

  if (!g_file_get_contents (filename, &contents, &len, error))
    return NULL;
  project = gj_project_from_xml (contents, len, dir, error);
  if (project == NULL)
    return NULL;
  project->filename = g_steal_pointer (&filename);
  project->dir = g_steal_pointer (&dir);
  return project;
}
