/* gj-window.c - GarageJam main window
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <string.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "gj-engine.h"
#include "gj-project.h"
#include "gj-waveform.h"
#include "gj-window.h"

struct _GjWindow {
  AdwApplicationWindow  parent_instance;

  GjProject            *project;
  GjEngine             *engine;
  gboolean              dirty;
  gboolean              closing;     /* close after a successful save */
  gboolean              syncing_arm; /* guard against toggle recursion */
  GjTrack              *recording_track;
  double                playhead;    /* seconds */
  double                timeline;    /* seconds shown across the width */
  guint                 tick_id;
  guint                 tick_count;

  AdwWindowTitle       *title;
  AdwToastOverlay      *toasts;
  GtkStack             *stack;
  GtkListBox           *list;
  GtkWidget            *record_button;
  GtkWidget            *play_button;
  GtkWidget            *stop_button;
  GtkLabel             *time_label;
  GtkLabel             *info_label;
  GtkLevelBar          *in_meter;
  GtkLevelBar          *out_meter;
};

G_DEFINE_FINAL_TYPE (GjWindow, gj_window, ADW_TYPE_APPLICATION_WINDOW)

static void rebuild_tracks   (GjWindow *self);
static void update_ui_state  (GjWindow *self);
static void update_timeline  (GjWindow *self);

/* ---------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------- */

static void
toast (GjWindow *self, const char *format, ...) G_GNUC_PRINTF (2, 3);

static void
toast (GjWindow *self, const char *format, ...)
{
  va_list args;
  g_autofree char *msg = NULL;

  va_start (args, format);
  msg = g_strdup_vprintf (format, args);
  va_end (args);
  adw_toast_overlay_add_toast (self->toasts, adw_toast_new (msg));
}

static void
set_dirty (GjWindow *self, gboolean dirty)
{
  self->dirty = dirty;
  update_ui_state (self);
}

static double
db_to_meter (double db)
{
  if (!isfinite (db))
    return 0.0;
  return CLAMP ((db + 60.0) / 60.0, 0.0, 1.0);
}

static void
format_time (char *buf, gsize len, double seconds)
{
  int minutes, whole;
  double secs;

  seconds = MAX (seconds, 0.0);
  minutes = (int) (seconds / 60.0);
  secs = seconds - minutes * 60.0;
  whole = (int) secs;
  g_snprintf (buf, len, "%02d:%02d.%d", minutes, whole,
              (int) ((secs - whole) * 10.0));
}

static void
set_action_enabled (GjWindow *self, const char *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self), name);
  if (action)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

static GtkWidget *
row_for_track (GjWindow *self, GjTrack *track)
{
  for (GtkWidget *row = gtk_widget_get_first_child (GTK_WIDGET (self->list));
       row; row = gtk_widget_get_next_sibling (row))
    if (g_object_get_data (G_OBJECT (row), "track") == track)
      return row;
  return NULL;
}

/* ---------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------- */

static void
update_ui_state (GjWindow *self)
{
  GjEngineState st = gj_engine_get_state (self->engine);
  gboolean idle = st == GJ_ENGINE_IDLE;
  g_autofree char *title = NULL;
  g_autofree char *subtitle = NULL;
  g_autofree char *info = NULL;

  title = g_strdup_printf ("%s%s", self->dirty ? "• " : "", self->project->title);
  if (self->project->filename)
    subtitle = g_path_get_basename (self->project->filename);
  else
    subtitle = g_strdup (_("Not saved"));
  adw_window_title_set_title (self->title, title);
  adw_window_title_set_subtitle (self->title, subtitle);

  for (const char **a = (const char *[]) { "new", "open", "add-track",
                                           "import", "export", NULL }; *a; a++)
    set_action_enabled (self, *a, idle);
  set_action_enabled (self, "record", idle);
  set_action_enabled (self, "play", idle);
  set_action_enabled (self, "stop", !idle && st != GJ_ENGINE_STOPPING);

  if (st == GJ_ENGINE_RECORDING)
    gtk_widget_add_css_class (self->record_button, "gj-recording");
  else
    gtk_widget_remove_css_class (self->record_button, "gj-recording");

  gtk_stack_set_visible_child_name (self->stack,
                                    self->project->tracks->len ? "tracks" : "empty");

  info = g_strdup_printf ("%.1f kHz · %s · %u %s",
                          self->project->rate / 1000.0,
                          self->project->channels == 1 ? _("Mono") : _("Stereo"),
                          self->project->tracks->len,
                          self->project->tracks->len == 1 ? _("track") : _("tracks"));
  gtk_label_set_text (self->info_label, info);
}

static double
longest_take (GjWindow *self)
{
  double longest = 0.0;

  for (GtkWidget *row = gtk_widget_get_first_child (GTK_WIDGET (self->list));
       row; row = gtk_widget_get_next_sibling (row))
    {
      GjWaveform *wf = g_object_get_data (G_OBJECT (row), "waveform");
      if (wf)
        longest = MAX (longest, gj_waveform_get_duration (wf));
    }
  return longest;
}

static void
update_timeline (GjWindow *self)
{
  double longest = longest_take (self);
  char buf[32];

  /* Keep some headroom so new takes have space to grow into. */
  longest = MAX (longest * 1.1, self->playhead + 5.0);
  if (longest > self->timeline || longest < self->timeline * 0.5)
    self->timeline = MAX (30.0, ceil (longest / 10.0) * 10.0);

  for (GtkWidget *row = gtk_widget_get_first_child (GTK_WIDGET (self->list));
       row; row = gtk_widget_get_next_sibling (row))
    {
      GjWaveform *wf = g_object_get_data (G_OBJECT (row), "waveform");
      GjTrack *track = g_object_get_data (G_OBJECT (row), "track");
      if (wf)
        {
          gj_waveform_set_timeline (wf, self->timeline, self->playhead);
          gj_waveform_set_dimmed (wf, track->path != NULL &&
                                      !gj_project_track_audible (self->project, track) &&
                                      track != self->recording_track);
        }
    }

  format_time (buf, sizeof buf, self->playhead);
  gtk_label_set_text (self->time_label, buf);
}

static gboolean
on_tick (gpointer user_data)
{
  GjWindow *self = user_data;
  gint64 pos = gj_engine_get_position (self->engine);

  if (pos >= 0)
    self->playhead = (double) pos / GST_SECOND;

  /* Refresh the waveform of the take being recorded twice a second, as
   * long as the file stays small enough to re-read cheaply. */
  if (self->recording_track && (++self->tick_count % 10) == 0)
    {
      GtkWidget *row = row_for_track (self, self->recording_track);
      const char *path = row ? g_object_get_data (G_OBJECT (row), "take-path") : NULL;
      GStatBuf st;

      if (path && g_stat (path, &st) == 0 && st.st_size < 64 * 1024 * 1024)
        gj_waveform_set_file (g_object_get_data (G_OBJECT (row), "waveform"), path);
    }

  update_timeline (self);
  return G_SOURCE_CONTINUE;
}

static void
start_tick (GjWindow *self)
{
  if (self->tick_id == 0)
    self->tick_id = g_timeout_add (50, on_tick, self);
}

static void
stop_tick (GjWindow *self)
{
  g_clear_handle_id (&self->tick_id, g_source_remove);
  gtk_level_bar_set_value (self->in_meter, 0);
  gtk_level_bar_set_value (self->out_meter, 0);
}

/* ---------------------------------------------------------------------
 * Engine callbacks
 * ------------------------------------------------------------------- */

static void
on_engine_level (GjEngine *engine, GjMeter meter, double peak_db,
                 gpointer user_data)
{
  GjWindow *self = user_data;
  gtk_level_bar_set_value (meter == GJ_METER_INPUT ? self->in_meter
                                                   : self->out_meter,
                           db_to_meter (peak_db));
}

static void
on_engine_finished (GjEngine *engine, GjEngineState what, const GError *error,
                    const char *record_path, gpointer user_data)
{
  GjWindow *self = user_data;
  GjTrack *track = self->recording_track;

  stop_tick (self);
  self->recording_track = NULL;

  if (track)
    {
      GtkWidget *row = row_for_track (self, track);
      if (row)
        {
          gj_waveform_set_recording (g_object_get_data (G_OBJECT (row), "waveform"),
                                     FALSE);
          g_object_set_data (G_OBJECT (row), "take-path", NULL);
        }
    }

  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    toast (self, _("Audio error: %s"), error->message);

  switch (what)
    {
    case GJ_ENGINE_RECORDING:
      if (record_path && track)
        {
          g_autofree char *base = g_path_get_basename (record_path);
          GtkWidget *row = row_for_track (self, track);

          g_free (track->path);
          track->path = g_strdup (record_path);
          if (row)
            {
              gj_waveform_set_file (g_object_get_data (G_OBJECT (row), "waveform"),
                                    record_path);
              gtk_widget_set_tooltip_text (g_object_get_data (G_OBJECT (row), "waveform"),
                                           record_path);
            }
          self->dirty = TRUE;
          toast (self, _("Recorded %s"), base);
        }
      break;
    case GJ_ENGINE_EXPORTING:
      if (error == NULL)
        toast (self, _("Mixdown exported"));
      else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        toast (self, _("Export cancelled"));
      break;
    default:
      break;
    }

  update_timeline (self);
  update_ui_state (self);
}

/* ---------------------------------------------------------------------
 * Track rows
 * ------------------------------------------------------------------- */

static void
on_name_changed (GtkEditable *editable, GjWindow *self)
{
  GjTrack *track = g_object_get_data (G_OBJECT (editable), "track");
  const char *text = gtk_editable_get_text (editable);

  if (g_strcmp0 (track->name, text) == 0)
    return;
  g_free (track->name);
  track->name = g_strdup (text);
  set_dirty (self, TRUE);
}

static void
on_arm_toggled (GtkToggleButton *button, GjWindow *self)
{
  GjTrack *track = g_object_get_data (G_OBJECT (button), "track");
  gboolean active = gtk_toggle_button_get_active (button);

  if (self->syncing_arm)
    return;

  track->armed = active;
  if (!active)
    return;

  /* Only one track can be armed at a time. */
  self->syncing_arm = TRUE;
  for (GtkWidget *row = gtk_widget_get_first_child (GTK_WIDGET (self->list));
       row; row = gtk_widget_get_next_sibling (row))
    {
      GjTrack *other = g_object_get_data (G_OBJECT (row), "track");
      GtkToggleButton *arm = g_object_get_data (G_OBJECT (row), "arm");
      if (other && other != track)
        {
          other->armed = FALSE;
          gtk_toggle_button_set_active (arm, FALSE);
        }
    }
  self->syncing_arm = FALSE;
}

static void
on_mix_toggled (GtkToggleButton *button, GjWindow *self)
{
  GjTrack *track = g_object_get_data (G_OBJECT (button), "track");
  const char *what = g_object_get_data (G_OBJECT (button), "what");
  gboolean active = gtk_toggle_button_get_active (button);

  if (g_str_equal (what, "mute"))
    track->mute = active;
  else
    track->solo = active;
  gj_engine_update_mix (self->engine, self->project);
  update_timeline (self);
  set_dirty (self, TRUE);
}

static void
on_scale_changed (GtkRange *range, GjWindow *self)
{
  GjTrack *track = g_object_get_data (G_OBJECT (range), "track");
  const char *what = g_object_get_data (G_OBJECT (range), "what");
  double v = gtk_range_get_value (range);

  if (g_str_equal (what, "volume"))
    track->volume = v;
  else
    track->pan = v;
  gj_engine_update_mix (self->engine, self->project);
  set_dirty (self, TRUE);
}

static void
on_remove_clicked (GtkButton *button, GjWindow *self)
{
  GjTrack *track = g_object_get_data (G_OBJECT (button), "track");

  if (gj_engine_get_state (self->engine) != GJ_ENGINE_IDLE)
    {
      toast (self, _("Stop the transport before removing tracks"));
      return;
    }
  /* The take on disk is kept; only the track is removed from the jam. */
  gj_project_remove_track (self->project, track);
  rebuild_tracks (self);
  set_dirty (self, TRUE);
}

static void
on_waveform_seek (GjWaveform *wf, double seconds, GjWindow *self)
{
  GjEngineState st = gj_engine_get_state (self->engine);

  if (st == GJ_ENGINE_RECORDING || st == GJ_ENGINE_STOPPING ||
      st == GJ_ENGINE_EXPORTING)
    return;

  self->playhead = seconds;
  if (st == GJ_ENGINE_PLAYING)
    {
      g_autoptr (GError) error = NULL;
      gj_engine_stop (self->engine);
      self->playhead = seconds;
      if (gj_engine_play (self->engine, self->project,
                          (gint64) (seconds * GST_SECOND), &error))
        start_tick (self);
      else
        toast (self, "%s", error->message);
    }
  update_timeline (self);
  update_ui_state (self);
}

static GtkWidget *
tag (GtkWidget *w, GjTrack *track, const char *what)
{
  g_object_set_data (G_OBJECT (w), "track", track);
  if (what)
    g_object_set_data (G_OBJECT (w), "what", (gpointer) what);
  return w;
}

static GtkWidget *
make_toggle (GjTrack *track, const char *label, const char *icon,
             const char *tooltip, const char *css, gboolean active)
{
  GtkWidget *b = icon ? gtk_toggle_button_new () : gtk_toggle_button_new_with_label (label);

  if (icon)
    gtk_button_set_icon_name (GTK_BUTTON (b), icon);
  gtk_widget_set_tooltip_text (b, tooltip);
  gtk_widget_add_css_class (b, "circular");
  gtk_widget_add_css_class (b, css);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (b), active);
  return tag (b, track, NULL);
}

static GtkWidget *
make_track_row (GjWindow *self, GjTrack *track)
{
  GtkWidget *row = gtk_list_box_row_new ();
  GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *name = gtk_entry_new ();
  GtkWidget *arm, *mute, *solo, *remove, *vol, *pan, *vbox, *pbox;
  GtkWidget *waveform = gj_waveform_new ();

  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
  gtk_widget_set_margin_top (hbox, 8);
  gtk_widget_set_margin_bottom (hbox, 8);
  gtk_widget_set_margin_start (hbox, 10);
  gtk_widget_set_margin_end (hbox, 10);
  gtk_widget_set_size_request (controls, 230, -1);

  /* name */
  gtk_editable_set_text (GTK_EDITABLE (name), track->name);
  gtk_accessible_update_property (GTK_ACCESSIBLE (name),
                                  GTK_ACCESSIBLE_PROPERTY_LABEL, _("Track name"), -1);
  tag (name, track, NULL);
  g_signal_connect (name, "changed", G_CALLBACK (on_name_changed), self);
  gtk_box_append (GTK_BOX (controls), name);

  /* arm / mute / solo / remove */
  arm = make_toggle (track, NULL, "media-record-symbolic",
                     _("Arm for recording"), "gj-arm", track->armed);
  mute = make_toggle (track, "M", NULL, _("Mute"), "gj-mute", track->mute);
  solo = make_toggle (track, "S", NULL, _("Solo"), "gj-solo", track->solo);
  g_object_set_data (G_OBJECT (mute), "what", (gpointer) "mute");
  g_object_set_data (G_OBJECT (solo), "what", (gpointer) "solo");
  g_signal_connect (arm, "toggled", G_CALLBACK (on_arm_toggled), self);
  g_signal_connect (mute, "toggled", G_CALLBACK (on_mix_toggled), self);
  g_signal_connect (solo, "toggled", G_CALLBACK (on_mix_toggled), self);

  remove = tag (gtk_button_new_from_icon_name ("user-trash-symbolic"), track, NULL);
  gtk_widget_add_css_class (remove, "flat");
  gtk_widget_set_tooltip_text (remove, _("Remove track"));
  gtk_widget_set_hexpand (remove, TRUE);
  gtk_widget_set_halign (remove, GTK_ALIGN_END);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_remove_clicked), self);

  gtk_box_append (GTK_BOX (buttons), arm);
  gtk_box_append (GTK_BOX (buttons), mute);
  gtk_box_append (GTK_BOX (buttons), solo);
  gtk_box_append (GTK_BOX (buttons), remove);
  gtk_box_append (GTK_BOX (controls), buttons);

  /* volume */
  vbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append (GTK_BOX (vbox), gtk_image_new_from_icon_name ("audio-volume-high-symbolic"));
  vol = tag (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 0.0, 2.0, 0.01),
             track, "volume");
  gtk_scale_add_mark (GTK_SCALE (vol), 1.0, GTK_POS_BOTTOM, NULL);
  gtk_range_set_value (GTK_RANGE (vol), track->volume);
  gtk_widget_set_hexpand (vol, TRUE);
  gtk_widget_set_tooltip_text (vol, _("Volume"));
  g_signal_connect (vol, "value-changed", G_CALLBACK (on_scale_changed), self);
  gtk_box_append (GTK_BOX (vbox), vol);
  gtk_box_append (GTK_BOX (controls), vbox);

  /* pan */
  pbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_append (GTK_BOX (pbox), gtk_label_new ("L"));
  pan = tag (gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, -1.0, 1.0, 0.01),
             track, "pan");
  gtk_scale_add_mark (GTK_SCALE (pan), 0.0, GTK_POS_BOTTOM, NULL);
  gtk_range_set_value (GTK_RANGE (pan), track->pan);
  gtk_widget_set_hexpand (pan, TRUE);
  gtk_widget_set_tooltip_text (pan, _("Pan"));
  g_signal_connect (pan, "value-changed", G_CALLBACK (on_scale_changed), self);
  gtk_box_append (GTK_BOX (pbox), pan);
  gtk_box_append (GTK_BOX (pbox), gtk_label_new ("R"));
  gtk_box_append (GTK_BOX (controls), pbox);

  /* waveform */
  gj_waveform_set_file (GJ_WAVEFORM (waveform), track->path);
  g_signal_connect (waveform, "seek", G_CALLBACK (on_waveform_seek), self);
  gtk_widget_add_css_class (waveform, "card");

  gtk_box_append (GTK_BOX (hbox), controls);
  gtk_box_append (GTK_BOX (hbox), waveform);
  gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), hbox);

  g_object_set_data (G_OBJECT (row), "track", track);
  g_object_set_data (G_OBJECT (row), "waveform", waveform);
  g_object_set_data (G_OBJECT (row), "arm", arm);
  if (track->path)
    gtk_widget_set_tooltip_text (waveform, track->path);
  return row;
}

static void
rebuild_tracks (GjWindow *self)
{
  gtk_list_box_remove_all (self->list);
  for (guint i = 0; i < self->project->tracks->len; i++)
    gtk_list_box_append (self->list,
                         make_track_row (self, g_ptr_array_index (self->project->tracks, i)));
  self->timeline = 0;
  update_timeline (self);
  update_ui_state (self);
}

static void
set_project (GjWindow *self, GjProject *project)
{
  if (gj_engine_get_state (self->engine) != GJ_ENGINE_IDLE)
    gj_engine_stop (self->engine);
  gj_project_free (self->project);
  self->project = project;
  self->playhead = 0;
  self->dirty = FALSE;
  rebuild_tracks (self);
}

/* ---------------------------------------------------------------------
 * Transport actions
 * ------------------------------------------------------------------- */

static void
action_play (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  GjWindow *self = user_data;
  g_autoptr (GError) error = NULL;

  /* At (or past) the end of the material: start again from the top.
   * longest_take() is 0 for formats without a peak preview. */
  {
    double longest = longest_take (self);
    if (longest > 0 && self->playhead >= longest - 0.05)
      self->playhead = 0;
  }
  if (!gj_engine_play (self->engine, self->project,
                       (gint64) (self->playhead * GST_SECOND), &error))
    {
      toast (self, "%s", error->message);
      return;
    }
  start_tick (self);
  update_ui_state (self);
}

static void
action_record (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  GjWindow *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *take = NULL;
  GjTrack *target = NULL;
  GtkWidget *row;

  for (guint i = 0; i < self->project->tracks->len; i++)
    {
      GjTrack *t = g_ptr_array_index (self->project->tracks, i);
      if (t->armed)
        target = t;
    }

  /* No armed track: record into a fresh one. */
  if (target == NULL)
    {
      target = gj_project_add_track (self->project, NULL);
      target->armed = TRUE;
      rebuild_tracks (self);
    }

  if (!gj_project_ensure_dir (self->project, &error) ||
      (take = gj_project_new_take_path (self->project, target)) == NULL)
    {
      toast (self, "%s", error ? error->message : _("Could not choose a take file name"));
      return;
    }

  /* While recording, the armed track is silent; the take replaces it. */
  {
    gboolean was_mute = target->mute;
    target->mute = TRUE;
    if (!gj_engine_record (self->engine, self->project, take, &error))
      {
        target->mute = was_mute;
        toast (self, "%s", error->message);
        return;
      }
    target->mute = was_mute;
  }

  self->recording_track = target;
  self->playhead = 0;
  self->tick_count = 0;
  row = row_for_track (self, target);
  if (row)
    {
      g_object_set_data_full (G_OBJECT (row), "take-path", g_strdup (take), g_free);
      gj_waveform_set_recording (g_object_get_data (G_OBJECT (row), "waveform"), TRUE);
    }
  start_tick (self);
  update_ui_state (self);
}

static void
action_stop (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  GjWindow *self = user_data;
  GjEngineState st = gj_engine_get_state (self->engine);

  gj_engine_stop (self->engine);
  if (st == GJ_ENGINE_IDLE)
    self->playhead = 0;   /* second stop returns to the start */
  update_timeline (self);
  update_ui_state (self);
}

static void
action_toggle_play (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  GjWindow *self = user_data;

  if (gj_engine_get_state (self->engine) == GJ_ENGINE_IDLE)
    action_play (NULL, NULL, self);
  else
    action_stop (NULL, NULL, self);
}

static void
action_add_track (GSimpleAction *a, GVariant *p, gpointer user_data)
{
  GjWindow *self = user_data;

  gj_project_add_track (self->project, NULL);
  rebuild_tracks (self);
  set_dirty (self, TRUE);
}

/* ---------------------------------------------------------------------
 * File actions
 * ------------------------------------------------------------------- */

static GtkFileFilter *
filter_new (const char *name, const char *mime, const char *pattern)
{
  GtkFileFilter *f = gtk_file_filter_new ();
  gtk_file_filter_set_name (f, name);
  if (mime)
    gtk_file_filter_add_mime_type (f, mime);
  if (pattern)
    gtk_file_filter_add_pattern (f, pattern);
  return f;
}

static GListModel *
filters_of (GtkFileFilter *first, ...)
{
  GListStore *store = g_list_store_new (GTK_TYPE_FILE_FILTER);
  va_list args;

  va_start (args, first);
  for (GtkFileFilter *f = first; f; f = va_arg (args, GtkFileFilter *))
    {
      g_list_store_append (store, f);
      g_object_unref (f);
    }
  va_end (args);
  return G_LIST_MODEL (store);
}

gboolean
gj_window_open_file (GjWindow *self, GFile *file)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *path = g_file_get_path (file);
  GjProject *project;

  if (path == NULL)
    {
      toast (self, _("Only local files can be opened"));
      return FALSE;
    }
  project = gj_project_load (path, &error);
  if (project == NULL)
    {
      toast (self, _("Could not open %s: %s"), path, error->message);
      return FALSE;
    }
  set_project (self, project);
  return TRUE;
}

static void
open_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GjWindow *self = user_data;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source),
                                                        res, NULL);
  if (file)
    gj_window_open_file (self, file);
}

static void
do_open (GjWindow *self)
{
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GListModel) filters =
    filters_of (filter_new (_("GarageJam projects"), "application/x-gjxml", "*.gjxml"),
                filter_new (_("All files"), NULL, "*"), NULL);

  gtk_file_dialog_set_title (dialog, _("Open Jam"));
  gtk_file_dialog_set_filters (dialog, filters);
  gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL, open_done, self);
}

static gboolean
save_to (GjWindow *self, const char *path)
{
  g_autoptr (GError) error = NULL;
  g_autofree char *base = g_path_get_basename (path);
  char *dot = strrchr (base, '.');

  if (dot && g_ascii_strcasecmp (dot, ".gjxml") == 0)
    *dot = '\0';
  g_free (self->project->title);
  self->project->title = g_strdup (base);

  if (!gj_project_save (self->project, path, &error))
    {
      toast (self, _("Could not save: %s"), error->message);
      return FALSE;
    }
  set_dirty (self, FALSE);
  toast (self, _("Saved"));
  if (self->closing)
    gtk_window_destroy (GTK_WINDOW (self));
  return TRUE;
}

static void
save_as_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GjWindow *self = user_data;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source),
                                                        res, NULL);
  g_autofree char *path = NULL;

  if (file == NULL || (path = g_file_get_path (file)) == NULL)
    {
      self->closing = FALSE;
      return;
    }
  if (!g_str_has_suffix (path, ".gjxml"))
    {
      char *tmp = g_strconcat (path, ".gjxml", NULL);
      g_free (path);
      path = tmp;
    }
  save_to (self, path);
}

static void
do_save_as (GjWindow *self)
{
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autofree char *name = g_strdup_printf ("%s.gjxml", self->project->title);

  gtk_file_dialog_set_title (dialog, _("Save Jam"));
  gtk_file_dialog_set_initial_name (dialog, name);
  if (self->project->dir)
    {
      g_autoptr (GFile) folder = g_file_new_for_path (self->project->dir);
      if (g_file_query_exists (folder, NULL))
        gtk_file_dialog_set_initial_folder (dialog, folder);
    }
  gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, save_as_done, self);
}

static void
do_save (GjWindow *self)
{
  if (self->project->filename)
    save_to (self, self->project->filename);
  else
    do_save_as (self);
}

static void
import_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GjWindow *self = user_data;
  g_autoptr (GListModel) files =
    gtk_file_dialog_open_multiple_finish (GTK_FILE_DIALOG (source), res, NULL);

  if (files == NULL)
    return;
  for (guint i = 0; i < g_list_model_get_n_items (files); i++)
    {
      g_autoptr (GFile) file = g_list_model_get_item (files, i);
      g_autofree char *path = g_file_get_path (file);
      g_autofree char *base = NULL;
      GjTrack *track;
      char *dot;

      if (path == NULL)
        continue;
      base = g_path_get_basename (path);
      if ((dot = strrchr (base, '.')))
        *dot = '\0';
      track = gj_project_add_track (self->project, base);
      track->path = g_steal_pointer (&path);
    }
  rebuild_tracks (self);
  set_dirty (self, TRUE);
}

static void
export_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  GjWindow *self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source),
                                                        res, NULL);
  g_autofree char *path = NULL;

  if (file == NULL || (path = g_file_get_path (file)) == NULL)
    return;
  if (!gj_engine_export (self->engine, self->project, path, &error))
    {
      toast (self, "%s", error->message);
      return;
    }
  toast (self, _("Exporting mixdown…"));
  update_ui_state (self);
}

static void
action_file (GSimpleAction *action, GVariant *p, gpointer user_data)
{
  GjWindow *self = user_data;
  const char *name = g_action_get_name (G_ACTION (action));

  if (g_str_equal (name, "new"))
    set_project (self, gj_project_new ());
  else if (g_str_equal (name, "open"))
    do_open (self);
  else if (g_str_equal (name, "save"))
    do_save (self);
  else if (g_str_equal (name, "save-as"))
    do_save_as (self);
  else if (g_str_equal (name, "import"))
    {
      g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
      g_autoptr (GListModel) filters =
        filters_of (filter_new (_("Audio files"), "audio/*", NULL),
                    filter_new (_("All files"), NULL, "*"), NULL);
      gtk_file_dialog_set_title (dialog, _("Import Audio"));
      gtk_file_dialog_set_filters (dialog, filters);
      gtk_file_dialog_open_multiple (dialog, GTK_WINDOW (self), NULL,
                                     import_done, self);
    }
  else if (g_str_equal (name, "export"))
    {
      g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
      g_autofree char *fname = g_strdup_printf ("%s-mixdown.wav", self->project->title);
      gtk_file_dialog_set_title (dialog, _("Export Mixdown"));
      gtk_file_dialog_set_initial_name (dialog, fname);
      gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL, export_done, self);
    }
}

/* ---------------------------------------------------------------------
 * Close handling
 * ------------------------------------------------------------------- */

static void
on_close_response (AdwAlertDialog *dialog, const char *response, GjWindow *self)
{
  if (g_str_equal (response, "discard"))
    {
      self->dirty = FALSE;
      gtk_window_destroy (GTK_WINDOW (self));
    }
  else if (g_str_equal (response, "save"))
    {
      self->closing = TRUE;
      do_save (self);
    }
}

static gboolean
on_close_request (GtkWindow *window)
{
  GjWindow *self = GJ_WINDOW (window);
  AdwDialog *dialog;

  if (gj_engine_get_state (self->engine) == GJ_ENGINE_RECORDING)
    {
      toast (self, _("Stop recording before closing"));
      return TRUE;
    }
  if (!self->dirty)
    return FALSE;

  dialog = adw_alert_dialog_new (_("Save Changes?"),
                                 _("This jam has unsaved changes. Changes "
                                   "which are not saved will be lost."));
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", _("_Cancel"),
                                  "discard", _("_Discard"),
                                  "save", _("_Save"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "discard",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "save",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "save");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect (dialog, "response", G_CALLBACK (on_close_response), self);
  adw_dialog_present (dialog, GTK_WIDGET (self));
  return TRUE;
}

/* ---------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------- */

static const GActionEntry win_actions[] = {
  { "new",         action_file },
  { "open",        action_file },
  { "save",        action_file },
  { "save-as",     action_file },
  { "import",      action_file },
  { "export",      action_file },
  { "add-track",   action_add_track },
  { "play",        action_play },
  { "record",      action_record },
  { "stop",        action_stop },
  { "toggle-play", action_toggle_play },
};

static GtkWidget *
transport_button (const char *icon, const char *tooltip, const char *action)
{
  GtkWidget *b = gtk_button_new_from_icon_name (icon);
  gtk_widget_set_tooltip_text (b, tooltip);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (b), action);
  return b;
}

static GMenuModel *
make_primary_menu (void)
{
  GMenu *menu = g_menu_new ();
  GMenu *s1 = g_menu_new (), *s2 = g_menu_new (), *s3 = g_menu_new ();

  g_menu_append (s1, _("_New Jam"), "win.new");
  g_menu_append (s1, _("_Open…"), "win.open");
  g_menu_append (s1, _("_Save"), "win.save");
  g_menu_append (s1, _("Save _As…"), "win.save-as");
  g_menu_append (s2, _("_Import Audio…"), "win.import");
  g_menu_append (s2, _("_Export Mixdown…"), "win.export");
  g_menu_append (s3, _("_About GarageJam"), "app.about");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (s1));
  g_menu_append_section (menu, NULL, G_MENU_MODEL (s2));
  g_menu_append_section (menu, NULL, G_MENU_MODEL (s3));
  g_object_unref (s1);
  g_object_unref (s2);
  g_object_unref (s3);
  return G_MENU_MODEL (menu);
}

static GtkWidget *
make_meter (const char *label, GtkLevelBar **out)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *l = gtk_label_new (label);
  GtkWidget *bar = gtk_level_bar_new_for_interval (0.0, 1.0);

  gtk_widget_add_css_class (l, "dim-label");
  gtk_widget_add_css_class (l, "caption");
  gtk_widget_set_size_request (bar, 140, -1);
  gtk_widget_set_valign (bar, GTK_ALIGN_CENTER);
  gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (bar), GTK_LEVEL_BAR_OFFSET_LOW);
  gtk_level_bar_remove_offset_value (GTK_LEVEL_BAR (bar), GTK_LEVEL_BAR_OFFSET_HIGH);
  gtk_level_bar_add_offset_value (GTK_LEVEL_BAR (bar), GTK_LEVEL_BAR_OFFSET_LOW, 0.80);
  gtk_level_bar_add_offset_value (GTK_LEVEL_BAR (bar), GTK_LEVEL_BAR_OFFSET_HIGH, 0.95);
  gtk_box_append (GTK_BOX (box), l);
  gtk_box_append (GTK_BOX (box), bar);
  *out = GTK_LEVEL_BAR (bar);
  return box;
}

static void
gj_window_dispose (GObject *object)
{
  GjWindow *self = GJ_WINDOW (object);

  g_clear_handle_id (&self->tick_id, g_source_remove);
  g_clear_pointer (&self->engine, gj_engine_free);
  g_clear_pointer (&self->project, gj_project_free);
  G_OBJECT_CLASS (gj_window_parent_class)->dispose (object);
}

static void
gj_window_class_init (GjWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = gj_window_dispose;
  GTK_WINDOW_CLASS (klass)->close_request = on_close_request;
}

static void
gj_window_init (GjWindow *self)
{
  GtkWidget *toolbar_view = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *transport = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *menu_button = gtk_menu_button_new ();
  GtkWidget *add_button;
  GtkWidget *status, *start_button, *scrolled, *clamp, *bottom;
  g_autoptr (GMenuModel) menu = make_primary_menu ();

  self->project = gj_project_new ();
  self->engine = gj_engine_new ();
  self->timeline = 30.0;
  gj_engine_set_callbacks (self->engine, on_engine_level, on_engine_finished, self);

  g_action_map_add_action_entries (G_ACTION_MAP (self), win_actions,
                                   G_N_ELEMENTS (win_actions), self);

  gtk_window_set_title (GTK_WINDOW (self), "GarageJam");
  gtk_window_set_default_size (GTK_WINDOW (self), 1100, 720);
  gtk_widget_set_size_request (GTK_WIDGET (self), 640, 400);

  /* header bar */
  self->record_button = transport_button ("media-record-symbolic", _("Record (Ctrl+R)"),
                                          "win.record");
  self->play_button = transport_button ("media-playback-start-symbolic",
                                        _("Play (Ctrl+Space)"), "win.play");
  self->stop_button = transport_button ("media-playback-stop-symbolic",
                                        _("Stop"), "win.stop");
  gtk_widget_add_css_class (self->record_button, "gj-record");
  gtk_widget_add_css_class (transport, "linked");
  gtk_box_append (GTK_BOX (transport), self->record_button);
  gtk_box_append (GTK_BOX (transport), self->play_button);
  gtk_box_append (GTK_BOX (transport), self->stop_button);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), transport);

  self->title = ADW_WINDOW_TITLE (adw_window_title_new ("GarageJam", NULL));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header), GTK_WIDGET (self->title));

  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_button), "open-menu-symbolic");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_button), menu);
  gtk_menu_button_set_primary (GTK_MENU_BUTTON (menu_button), TRUE);
  gtk_widget_set_tooltip_text (menu_button, _("Main Menu"));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_button);

  add_button = transport_button ("list-add-symbolic", _("Add Track (Ctrl+T)"),
                                 "win.add-track");
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), add_button);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  /* content */
  self->stack = GTK_STACK (gtk_stack_new ());
  gtk_stack_set_transition_type (self->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);

  status = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status), "org.garagejam.GarageJam-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (status), _("Start a Jam"));
  adw_status_page_set_description (ADW_STATUS_PAGE (status),
                                   _("Press record to lay down your first take, "
                                     "or add tracks and import audio"));
  start_button = gtk_button_new_with_mnemonic (_("_Add Track"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (start_button), "win.add-track");
  gtk_widget_add_css_class (start_button, "pill");
  gtk_widget_add_css_class (start_button, "suggested-action");
  gtk_widget_set_halign (start_button, GTK_ALIGN_CENTER);
  adw_status_page_set_child (ADW_STATUS_PAGE (status), start_button);
  gtk_stack_add_named (self->stack, status, "empty");

  self->list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (self->list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class (GTK_WIDGET (self->list), "boxed-list");
  gtk_widget_set_valign (GTK_WIDGET (self->list), GTK_ALIGN_START);
  clamp = adw_clamp_new ();
  adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 1600);
  gtk_widget_set_margin_top (clamp, 12);
  gtk_widget_set_margin_bottom (clamp, 12);
  gtk_widget_set_margin_start (clamp, 12);
  gtk_widget_set_margin_end (clamp, 12);
  adw_clamp_set_child (ADW_CLAMP (clamp), GTK_WIDGET (self->list));
  scrolled = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), clamp);
  gtk_stack_add_named (self->stack, scrolled, "tracks");

  self->toasts = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
  adw_toast_overlay_set_child (self->toasts, GTK_WIDGET (self->stack));
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), GTK_WIDGET (self->toasts));

  /* bottom transport bar */
  bottom = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18);
  gtk_widget_add_css_class (bottom, "toolbar");
  gtk_widget_set_margin_start (bottom, 6);
  gtk_widget_set_margin_end (bottom, 6);
  self->time_label = GTK_LABEL (gtk_label_new ("00:00.0"));
  gtk_widget_add_css_class (GTK_WIDGET (self->time_label), "gj-clock");
  gtk_widget_add_css_class (GTK_WIDGET (self->time_label), "numeric");
  gtk_box_append (GTK_BOX (bottom), GTK_WIDGET (self->time_label));
  gtk_box_append (GTK_BOX (bottom), make_meter (_("In"), &self->in_meter));
  gtk_box_append (GTK_BOX (bottom), make_meter (_("Out"), &self->out_meter));
  self->info_label = GTK_LABEL (gtk_label_new (NULL));
  gtk_widget_add_css_class (GTK_WIDGET (self->info_label), "dim-label");
  gtk_widget_set_hexpand (GTK_WIDGET (self->info_label), TRUE);
  gtk_widget_set_halign (GTK_WIDGET (self->info_label), GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (bottom), GTK_WIDGET (self->info_label));
  adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar_view), bottom);

  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self), toolbar_view);

  rebuild_tracks (self);
}

GjWindow *
gj_window_new (AdwApplication *app)
{
  return g_object_new (GJ_TYPE_WINDOW, "application", app, NULL);
}
