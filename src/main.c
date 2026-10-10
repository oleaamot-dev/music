/* main.c - GarageJam multitrack music recording studio
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <locale.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gst/gst.h>
#include <adwaita.h>

#include "gj-engine.h"
#include "gj-project.h"
#include "gj-window.h"

static const char *app_css =
  ".gj-record { color: @error_color; }\n"
  ".gj-record.gj-recording, .gj-record.gj-recording:disabled {\n"
  "  background: @error_bg_color; color: @error_fg_color; opacity: 1; }\n"
  ".gj-arm:checked { background: @error_bg_color; color: @error_fg_color; }\n"
  ".gj-mute:checked { background: @warning_bg_color; color: @warning_fg_color; }\n"
  ".gj-solo:checked { background: @success_bg_color; color: @success_fg_color; }\n"
  ".gj-clock { font-size: 1.6em; font-weight: 700; min-width: 6.5em; }\n"
  "gj-waveform { border-radius: 8px; }\n";

/* ---------------------------------------------------------------------
 * Headless mixdown:  garagejam --export-mixdown OUT.wav PROJECT.gjxml
 * ------------------------------------------------------------------- */

typedef struct {
  GMainLoop *loop;
  int        status;
} HeadlessExport;

static void
headless_finished (GjEngine *engine, GjEngineState what, const GError *error,
                   const char *record_path, gpointer user_data)
{
  HeadlessExport *he = user_data;

  if (error)
    {
      g_printerr ("garagejam: export failed: %s\n", error->message);
      he->status = 1;
    }
  g_main_loop_quit (he->loop);
}

static int
headless_export (const char *project_path, const char *wav_path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GjProject) project = gj_project_load (project_path, &error);
  HeadlessExport he = { NULL, 0 };
  GjEngine *engine;

  if (project == NULL)
    {
      g_printerr ("garagejam: %s: %s\n", project_path, error->message);
      return 1;
    }

  engine = gj_engine_new ();
  he.loop = g_main_loop_new (NULL, FALSE);
  gj_engine_set_callbacks (engine, NULL, headless_finished, &he);
  if (!gj_engine_export (engine, project, wav_path, &error))
    {
      g_printerr ("garagejam: %s\n", error->message);
      he.status = 1;
    }
  else
    g_main_loop_run (he.loop);

  gj_engine_free (engine);
  g_main_loop_unref (he.loop);
  if (he.status == 0)
    g_print ("garagejam: wrote %s\n", wav_path);
  return he.status;
}

static int
on_handle_local_options (GApplication *app, GVariantDict *options,
                         gpointer user_data)
{
  const char *project = NULL;
  const char *out = NULL;
  g_autofree char *default_out = NULL;

  if (g_variant_dict_contains (options, "version"))
    {
      g_print ("%s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
      return 0;
    }

  if (g_variant_dict_lookup (options, "export-mixdown", "^&ay", &project))
    {
      if (!g_variant_dict_lookup (options, "output", "^&ay", &out))
        {
          g_autofree char *stem = g_strdup (project);
          char *dot = strrchr (stem, '.');
          if (dot && strchr (dot, G_DIR_SEPARATOR) == NULL)
            *dot = '\0';
          out = default_out = g_strconcat (stem, "-mixdown.wav", NULL);
        }
      return headless_export (project, out);
    }

  return -1;   /* continue: activate, or open any PROJECT.gjxml arguments */
}

/* ---------------------------------------------------------------------
 * Application
 * ------------------------------------------------------------------- */

static void
action_about (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  GtkApplication *app = user_data;
  static const char *developers[] = {
    "Ole Kristian Aamot <ole.aamot@aamotinnovation.com>", NULL
  };
  AdwDialog *about = adw_about_dialog_new ();

  adw_about_dialog_set_application_name (ADW_ABOUT_DIALOG (about), "GarageJam");
  adw_about_dialog_set_application_icon (ADW_ABOUT_DIALOG (about), "org.garagejam.GarageJam");
  adw_about_dialog_set_version (ADW_ABOUT_DIALOG (about), PACKAGE_VERSION);
  adw_about_dialog_set_developer_name (ADW_ABOUT_DIALOG (about), "Aamot Innovation");
  adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (about), developers);
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (about),
                                  "© 2026 Ole Kristian Aamot");
  adw_about_dialog_set_comments (ADW_ABOUT_DIALOG (about),
                                 _("Multitrack music recording for jam sessions"));
  adw_about_dialog_set_website (ADW_ABOUT_DIALOG (about), PACKAGE_URL);
  adw_about_dialog_set_issue_url (ADW_ABOUT_DIALOG (about), PACKAGE_BUGREPORT);
  adw_about_dialog_set_license_type (ADW_ABOUT_DIALOG (about), GTK_LICENSE_GPL_3_0);
  adw_dialog_present (about, GTK_WIDGET (gtk_application_get_active_window (app)));
}

static void
action_quit (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  GtkApplication *app = user_data;
  GList *windows = g_list_copy (gtk_application_get_windows (app));

  /* close-request lets each window ask about unsaved changes */
  for (GList *l = windows; l; l = l->next)
    gtk_window_close (l->data);
  g_list_free (windows);
}

static void
on_startup (GApplication *app, gpointer user_data)
{
  static const GActionEntry app_actions[] = {
    { "about", action_about },
    { "quit",  action_quit },
  };
  static const struct { const char *action; const char *accels[2]; } accels[] = {
    { "app.quit",        { "<Primary>q", NULL } },
    { "win.new",         { "<Primary>n", NULL } },
    { "win.open",        { "<Primary>o", NULL } },
    { "win.save",        { "<Primary>s", NULL } },
    { "win.save-as",     { "<Primary><Shift>s", NULL } },
    { "win.import",      { "<Primary>i", NULL } },
    { "win.export",      { "<Primary>e", NULL } },
    { "win.add-track",   { "<Primary>t", NULL } },
    { "win.record",      { "<Primary>r", NULL } },
    { "win.toggle-play", { "<Primary>space", NULL } },
    { "window.close",    { "<Primary>w", NULL } },
  };
  GtkCssProvider *css = gtk_css_provider_new ();

  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions,
                                   G_N_ELEMENTS (app_actions), app);
  for (gsize i = 0; i < G_N_ELEMENTS (accels); i++)
    gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                           accels[i].action, accels[i].accels);

  gtk_css_provider_load_from_string (css, app_css);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (css);
  gtk_window_set_default_icon_name ("org.garagejam.GarageJam");
}

static void
on_activate (GApplication *app, gpointer user_data)
{
  GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));

  if (win == NULL)
    win = GTK_WINDOW (gj_window_new (ADW_APPLICATION (app)));
  gtk_window_present (win);
}

static void
on_open (GApplication *app, GFile **files, int n_files, const char *hint,
         gpointer user_data)
{
  for (int i = 0; i < n_files; i++)
    {
      GjWindow *win = gj_window_new (ADW_APPLICATION (app));
      gj_window_open_file (win, files[i]);
      gtk_window_present (GTK_WINDOW (win));
    }
}

int
main (int argc, char *argv[])
{
  static const GOptionEntry entries[] = {
    { "export-mixdown", 'x', 0, G_OPTION_ARG_FILENAME, NULL,
      N_("Render a project to a WAV mixdown without opening a window"),
      N_("PROJECT.gjxml") },
    { "output", 'o', 0, G_OPTION_ARG_FILENAME, NULL,
      N_("Mixdown file name (default: PROJECT-mixdown.wav)"), N_("OUT.wav") },
    { "version", 'v', 0, G_OPTION_ARG_NONE, NULL,
      N_("Print the version and exit"), NULL },
    { NULL }
  };
  g_autoptr (AdwApplication) app = NULL;

  setlocale (LC_ALL, "");
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  gst_init (&argc, &argv);
  g_set_application_name ("GarageJam");

  app = adw_application_new ("org.garagejam.GarageJam", G_APPLICATION_HANDLES_OPEN);
  g_application_add_main_option_entries (G_APPLICATION (app), entries);
  g_application_set_option_context_summary (G_APPLICATION (app),
                                            _("GarageJam — multitrack music recording"));
  g_signal_connect (app, "handle-local-options", G_CALLBACK (on_handle_local_options), NULL);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  g_signal_connect (app, "open", G_CALLBACK (on_open), NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
