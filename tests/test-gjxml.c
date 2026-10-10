/* test-gjxml.c - GJXML round-trip tests
 * Copyright (C) 2026 Ole Kristian Aamot
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <locale.h>
#include <string.h>
#include <glib/gstdio.h>
#include "gj-project.h"

static void
test_roundtrip (void)
{
  g_autoptr (GjProject) p = gj_project_new ();
  g_autoptr (GjProject) q = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *xml = NULL;
  GjTrack *t;

  g_free (p->title);
  p->title = g_strdup ("Blåfisk & <Venner>");
  p->rate = 44100;
  p->bpm = 98.5;
  t = gj_project_add_track (p, "Bass \"DI\"");
  t->path = g_strdup ("/jam/bass-take1.wav");
  t->volume = 0.75;
  t->pan = -0.25;
  t->mute = TRUE;
  t = gj_project_add_track (p, NULL);
  t->solo = TRUE;

  xml = gj_project_to_xml (p, "/jam");
  g_assert_nonnull (strstr (xml, "file=\"bass-take1.wav\""));
  g_assert_nonnull (strstr (xml, "volume=\"0.750\""));

  q = gj_project_from_xml (xml, -1, "/elsewhere", &error);
  g_assert_no_error (error);
  g_assert_cmpstr (q->title, ==, "Blåfisk & <Venner>");
  g_assert_cmpint (q->rate, ==, 44100);
  g_assert_cmpfloat_with_epsilon (q->bpm, 98.5, 1e-6);
  g_assert_cmpuint (q->tracks->len, ==, 2);

  t = g_ptr_array_index (q->tracks, 0);
  g_assert_cmpstr (t->name, ==, "Bass \"DI\"");
  g_assert_cmpstr (t->path, ==, "/elsewhere/bass-take1.wav");
  g_assert_cmpfloat_with_epsilon (t->volume, 0.75, 1e-6);
  g_assert_cmpfloat_with_epsilon (t->pan, -0.25, 1e-6);
  g_assert_true (t->mute);
  g_assert_false (t->solo);

  t = g_ptr_array_index (q->tracks, 1);
  g_assert_cmpstr (t->name, ==, "Track 2");
  g_assert_null (t->path);
  g_assert_true (t->solo);
}

static void
test_locale_independent (void)
{
  /* Norwegian locales use a decimal comma; GJXML must not. */
  g_autoptr (GjProject) p = gj_project_new ();
  g_autofree char *xml = NULL;
  const char *old = setlocale (LC_NUMERIC, NULL);
  g_autofree char *saved = g_strdup (old);

  if (setlocale (LC_NUMERIC, "nb_NO.UTF-8") == NULL &&
      setlocale (LC_NUMERIC, "de_DE.UTF-8") == NULL)
    {
      g_test_skip ("no comma-decimal locale installed");
      return;
    }
  gj_project_add_track (p, "x")->volume = 1.5;
  xml = gj_project_to_xml (p, NULL);
  g_assert_nonnull (strstr (xml, "volume=\"1.500\""));
  setlocale (LC_NUMERIC, saved);
}

static void
test_rejects_bad_documents (void)
{
  g_autoptr (GError) error = NULL;
  GjProject *p;

  p = gj_project_from_xml ("<html/>", -1, NULL, &error);
  g_assert_null (p);
  g_assert_error (error, GJ_ERROR, GJ_ERROR_PARSE);
  g_clear_error (&error);

  p = gj_project_from_xml ("<garagejam><project", -1, NULL, &error);
  g_assert_null (p);
  g_assert_nonnull (error);
  g_clear_error (&error);

  p = gj_project_from_xml ("<garagejam/>", -1, NULL, &error);
  g_assert_null (p);
  g_assert_error (error, GJ_ERROR, GJ_ERROR_PARSE);
}

static void
test_clamps_and_ignores_unknown (void)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GjProject) p = gj_project_from_xml (
    "<garagejam version='9'><project rate='5' channels='7'>"
    "<track name='a' volume='99' pan='-9' mute='1'><future/></track>"
    "<marker at='1.0'/></project></garagejam>", -1, NULL, &error);
  GjTrack *t;

  g_assert_no_error (error);
  g_assert_cmpint (p->rate, ==, 8000);
  g_assert_cmpint (p->channels, ==, 2);
  t = g_ptr_array_index (p->tracks, 0);
  g_assert_cmpfloat (t->volume, ==, 2.0);
  g_assert_cmpfloat (t->pan, ==, -1.0);
  g_assert_true (t->mute);
}

static void
test_sample_file (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = g_test_build_filename (G_TEST_DIST, "data", "sample.gjxml", NULL);
  g_autoptr (GjProject) p = gj_project_load (path, &error);

  g_assert_no_error (error);
  g_assert_cmpstr (p->title, ==, "Sample Jam");
  g_assert_cmpuint (p->tracks->len, ==, 3);
  g_assert_nonnull (p->filename);
  g_assert_true (g_str_has_suffix (((GjTrack *) g_ptr_array_index (p->tracks, 0))->path,
                                   "data/drums-take1.wav"));
}

static void
test_take_names (void)
{
  g_autoptr (GjProject) p = gj_project_new ();
  g_autofree char *dir = g_dir_make_tmp ("gj-test-XXXXXX", NULL);
  g_autofree char *path1 = NULL, *path2 = NULL;
  GjTrack *t = gj_project_add_track (p, "Lead Vocal (Ole)!");

  p->dir = g_strdup (dir);
  path1 = gj_project_new_take_path (p, t);
  g_assert_true (g_str_has_suffix (path1, "/lead-vocal-ole-take1.wav"));
  g_file_set_contents (path1, "", 0, NULL);
  path2 = gj_project_new_take_path (p, t);
  g_assert_true (g_str_has_suffix (path2, "/lead-vocal-ole-take2.wav"));
  g_unlink (path1);
  g_rmdir (dir);
}

int
main (int argc, char *argv[])
{
  setlocale (LC_ALL, "");
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gjxml/roundtrip", test_roundtrip);
  g_test_add_func ("/gjxml/locale", test_locale_independent);
  g_test_add_func ("/gjxml/bad", test_rejects_bad_documents);
  g_test_add_func ("/gjxml/clamp", test_clamps_and_ignores_unknown);
  g_test_add_func ("/gjxml/sample", test_sample_file);
  g_test_add_func ("/project/take-names", test_take_names);
  return g_test_run ();
}
