/* gj-engine.c - GarageJam GStreamer audio engine
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pipelines built here:
 *
 *   playback:  [track branches] -> audiomixer -> caps -> audioconvert
 *                               -> level(out) -> autoaudiosink
 *   record:    playback (overdub, if any audible tracks)
 *              + autoaudiosrc -> audioconvert -> audioresample -> caps(S16LE)
 *                -> level(in) -> queue -> wavenc -> filesink
 *   export:    [track branches] -> audiomixer -> caps -> audioconvert
 *                               -> caps(S16LE) -> wavenc -> filesink
 *
 *   track branch: uridecodebin -> audioconvert -> audioresample
 *                 -> audiopanorama -> volume -> queue -> (mixer pad)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <string.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "gj-engine.h"

struct _GjEngine {
  GstElement           *pipeline;
  guint                 bus_watch;
  guint                 stop_timeout;
  GjEngineState         state;
  GjEngineState         mode;        /* what was started */
  gint64                start_ns;
  gboolean              started;
  char                 *record_path;
  char                 *export_path;

  GjEngineLevelFunc     level_func;
  GjEngineFinishedFunc  finished_func;
  gpointer              user_data;
};

GjEngine *
gj_engine_new (void)
{
  GjEngine *engine = g_new0 (GjEngine, 1);
  engine->state = GJ_ENGINE_IDLE;
  return engine;
}

void
gj_engine_set_callbacks (GjEngine *engine,
                         GjEngineLevelFunc level_func,
                         GjEngineFinishedFunc finished_func,
                         gpointer user_data)
{
  engine->level_func = level_func;
  engine->finished_func = finished_func;
  engine->user_data = user_data;
}

static void
teardown (GjEngine *engine)
{
  g_clear_handle_id (&engine->stop_timeout, g_source_remove);
  g_clear_handle_id (&engine->bus_watch, g_source_remove);
  if (engine->pipeline)
    {
      gst_element_set_state (engine->pipeline, GST_STATE_NULL);
      g_clear_object (&engine->pipeline);
    }
  engine->state = GJ_ENGINE_IDLE;
  engine->started = FALSE;
}

void
gj_engine_free (GjEngine *engine)
{
  if (engine == NULL)
    return;
  teardown (engine);
  g_free (engine->record_path);
  g_free (engine->export_path);
  g_free (engine);
}

GjEngineState
gj_engine_get_state (GjEngine *engine)
{
  return engine->state;
}

gint64
gj_engine_get_position (GjEngine *engine)
{
  gint64 pos = 0;

  if (engine->pipeline == NULL)
    return 0;
  if (!gst_element_query_position (engine->pipeline, GST_FORMAT_TIME, &pos))
    return -1;
  return pos;
}

static void
finish (GjEngine *engine, const GError *error)
{
  GjEngineState what = engine->mode;
  g_autofree char *take = NULL;

  if (what == GJ_ENGINE_RECORDING && error == NULL)
    take = g_steal_pointer (&engine->record_path);
  if (what == GJ_ENGINE_EXPORTING && error != NULL && engine->export_path)
    g_unlink (engine->export_path);

  g_clear_pointer (&engine->record_path, g_free);
  g_clear_pointer (&engine->export_path, g_free);
  teardown (engine);

  if (engine->finished_func)
    engine->finished_func (engine, what, error, take, engine->user_data);
}

/* ---------------------------------------------------------------------
 * Pipeline construction helpers
 * ------------------------------------------------------------------- */

static GstElement *
make (GstElement *bin, const char *factory, const char *name, GError **error)
{
  GstElement *e = gst_element_factory_make (factory, name);

  if (e == NULL)
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_PLUGIN,
                   "The GStreamer element “%s” is missing. Please install "
                   "the GStreamer base and good plugins.", factory);
      return NULL;
    }
  gst_bin_add (GST_BIN (bin), e);
  return e;
}

static GstCaps *
raw_caps (GjProject *project, const char *format)
{
  GstCaps *caps = gst_caps_new_simple ("audio/x-raw",
                                       "rate", G_TYPE_INT, project->rate,
                                       "channels", G_TYPE_INT, project->channels,
                                       NULL);
  if (format)
    gst_caps_set_simple (caps, "format", G_TYPE_STRING, format,
                         "layout", G_TYPE_STRING, "interleaved", NULL);
  return caps;
}

static void
on_pad_added (GstElement *decodebin, GstPad *pad, gpointer user_data)
{
  GstElement *convert = user_data;
  g_autoptr (GstPad) sink = gst_element_get_static_pad (convert, "sink");
  g_autoptr (GstCaps) caps = gst_pad_get_current_caps (pad);
  const char *name;

  if (gst_pad_is_linked (sink))
    return;
  if (caps == NULL)
    caps = gst_pad_query_caps (pad, NULL);
  name = gst_structure_get_name (gst_caps_get_structure (caps, 0));
  if (!g_str_has_prefix (name, "audio/"))
    return;
  if (gst_pad_link (pad, sink) != GST_PAD_LINK_OK)
    g_warning ("GarageJam: could not link decoded audio pad");
}

static void
apply_track_mix (GstElement *pipeline, GjProject *project, GjTrack *t)
{
  g_autofree char *vname = g_strdup_printf ("vol-%p", (void *) t);
  g_autofree char *pname = g_strdup_printf ("pan-%p", (void *) t);
  g_autoptr (GstElement) vol = gst_bin_get_by_name (GST_BIN (pipeline), vname);
  g_autoptr (GstElement) pan = gst_bin_get_by_name (GST_BIN (pipeline), pname);

  if (vol)
    g_object_set (vol, "volume", t->volume,
                  "mute", !gj_project_track_audible (project, t), NULL);
  if (pan)
    g_object_set (pan, "panorama", (float) t->pan, NULL);
}

/* Adds one branch per track that has audio into @mixer. Muted (or not
 * soloed) tracks are added with the volume element muted, so that mute,
 * solo, volume and pan can be changed live with gj_engine_update_mix().
 * Returns the number of audible tracks, or -1 on error. */
static int
add_track_branches (GstElement *pipeline, GstElement *mixer,
                    GjProject *project, GError **error)
{
  int audible = 0;

  for (guint i = 0; i < project->tracks->len; i++)
    {
      GjTrack *t = g_ptr_array_index (project->tracks, i);
      GstElement *src, *conv, *res, *pan, *vol, *queue;
      g_autofree char *uri = NULL;
      g_autofree char *vname = NULL, *pname = NULL;

      if (t->path == NULL || !g_file_test (t->path, G_FILE_TEST_IS_REGULAR))
        continue;

      uri = g_filename_to_uri (t->path, NULL, error);
      if (uri == NULL)
        return -1;

      vname = g_strdup_printf ("vol-%p", (void *) t);
      pname = g_strdup_printf ("pan-%p", (void *) t);
      if (!(src = make (pipeline, "uridecodebin", NULL, error)) ||
          !(conv = make (pipeline, "audioconvert", NULL, error)) ||
          !(res = make (pipeline, "audioresample", NULL, error)) ||
          !(pan = make (pipeline, "audiopanorama", pname, error)) ||
          !(vol = make (pipeline, "volume", vname, error)) ||
          !(queue = make (pipeline, "queue", NULL, error)))
        return -1;

      g_object_set (src, "uri", uri, NULL);
      g_signal_connect (src, "pad-added", G_CALLBACK (on_pad_added), conv);
      apply_track_mix (pipeline, project, t);

      if (!gst_element_link_many (conv, res, pan, vol, queue, mixer, NULL))
        {
          g_set_error (error, GJ_ERROR, GJ_ERROR_STREAM,
                       "Could not link track “%s” into the mixer", t->name);
          return -1;
        }
      if (gj_project_track_audible (project, t))
        audible++;
    }
  return audible;
}

void
gj_engine_update_mix (GjEngine *engine, GjProject *project)
{
  if (engine->pipeline == NULL)
    return;
  for (guint i = 0; i < project->tracks->len; i++)
    apply_track_mix (engine->pipeline, project,
                     g_ptr_array_index (project->tracks, i));
}

/* mixer -> caps(rate/channels) -> audioconvert ; returns the audioconvert */
static GstElement *
add_mixer (GstElement *pipeline, GjProject *project, GstElement **mixer_out,
           GError **error)
{
  GstElement *mixer, *caps, *conv;
  g_autoptr (GstCaps) c = raw_caps (project, NULL);

  if (!(mixer = make (pipeline, "audiomixer", "mixer", error)) ||
      !(caps = make (pipeline, "capsfilter", NULL, error)) ||
      !(conv = make (pipeline, "audioconvert", NULL, error)))
    return NULL;
  g_object_set (caps, "caps", c, NULL);
  if (!gst_element_link_many (mixer, caps, conv, NULL))
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_STREAM, "Could not build mixer");
      return NULL;
    }
  *mixer_out = mixer;
  return conv;
}

static gboolean
add_playback_output (GstElement *pipeline, GstElement *tail, GError **error)
{
  GstElement *level, *sink;

  if (!(level = make (pipeline, "level", "outlevel", error)) ||
      !(sink = make (pipeline, "autoaudiosink", NULL, error)))
    return FALSE;
  g_object_set (level, "interval", (guint64) 50 * GST_MSECOND,
                "post-messages", TRUE, NULL);
  if (!gst_element_link_many (tail, level, sink, NULL))
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_STREAM,
                   "Could not connect to the audio output");
      return FALSE;
    }
  return TRUE;
}

static gboolean
add_wav_output (GstElement *pipeline, GstElement *tail, GjProject *project,
                const char *path, GError **error)
{
  GstElement *caps, *enc, *sink;
  g_autoptr (GstCaps) c = raw_caps (project, "S16LE");

  if (!(caps = make (pipeline, "capsfilter", NULL, error)) ||
      !(enc = make (pipeline, "wavenc", NULL, error)) ||
      !(sink = make (pipeline, "filesink", NULL, error)))
    return FALSE;
  g_object_set (caps, "caps", c, NULL);
  g_object_set (sink, "location", path, NULL);
  if (!gst_element_link_many (tail, caps, enc, sink, NULL))
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_STREAM,
                   "Could not build the WAV writer");
      return FALSE;
    }
  return TRUE;
}

/* ---------------------------------------------------------------------
 * Bus handling
 * ------------------------------------------------------------------- */

/* "level" posts its per-channel values as a GValueArray (deprecated in
 * GLib, but it is what the element emits), newer releases may use a list. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static double
peak_from_message (const GstStructure *s)
{
  const GValue *v = gst_structure_get_value (s, "peak");
  double peak = -HUGE_VAL;

  if (v == NULL)
    return peak;

  if (GST_VALUE_HOLDS_LIST (v) || GST_VALUE_HOLDS_ARRAY (v))
    {
      guint n = GST_VALUE_HOLDS_LIST (v) ? gst_value_list_get_size (v)
                                         : gst_value_array_get_size (v);
      for (guint i = 0; i < n; i++)
        {
          const GValue *c = GST_VALUE_HOLDS_LIST (v) ? gst_value_list_get_value (v, i)
                                                     : gst_value_array_get_value (v, i);
          peak = MAX (peak, g_value_get_double (c));
        }
    }
  else if (G_VALUE_HOLDS_BOXED (v))
    {
      GValueArray *arr = g_value_get_boxed (v);
      for (guint i = 0; arr && i < arr->n_values; i++)
        peak = MAX (peak, g_value_get_double (g_value_array_get_nth (arr, i)));
    }
  return peak;
}
G_GNUC_END_IGNORE_DEPRECATIONS

static gboolean
on_bus_message (GstBus *bus, GstMessage *msg, gpointer user_data)
{
  GjEngine *engine = user_data;

  switch (GST_MESSAGE_TYPE (msg))
    {
    case GST_MESSAGE_EOS:
      finish (engine, NULL);
      return G_SOURCE_REMOVE;

    case GST_MESSAGE_ERROR:
      {
        g_autoptr (GError) err = NULL;
        g_autofree char *debug = NULL;

        gst_message_parse_error (msg, &err, &debug);
        g_debug ("GStreamer error: %s (%s)", err->message, debug ? debug : "");
        engine->bus_watch = 0;   /* we return REMOVE below */
        finish (engine, err);
        return G_SOURCE_REMOVE;
      }

    case GST_MESSAGE_ASYNC_DONE:
      if (engine->mode == GJ_ENGINE_PLAYING && !engine->started)
        {
          engine->started = TRUE;
          if (engine->start_ns > 0)
            gst_element_seek_simple (engine->pipeline, GST_FORMAT_TIME,
                                     GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                                     engine->start_ns);
          gst_element_set_state (engine->pipeline, GST_STATE_PLAYING);
        }
      break;

    case GST_MESSAGE_ELEMENT:
      {
        const GstStructure *s = gst_message_get_structure (msg);
        if (s && gst_structure_has_name (s, "level") && engine->level_func)
          {
            const char *src = GST_MESSAGE_SRC_NAME (msg);
            GjMeter meter = g_strcmp0 (src, "inlevel") == 0
                            ? GJ_METER_INPUT : GJ_METER_OUTPUT;
            engine->level_func (engine, meter, peak_from_message (s),
                                engine->user_data);
          }
      }
      break;

    default:
      break;
    }
  return G_SOURCE_CONTINUE;
}

static gboolean
start_pipeline (GjEngine *engine, GjEngineState mode, GstState target,
                GError **error)
{
  g_autoptr (GstBus) bus = gst_element_get_bus (engine->pipeline);

  engine->bus_watch = gst_bus_add_watch (bus, on_bus_message, engine);
  engine->mode = mode;
  engine->state = mode;
  engine->started = FALSE;

  if (gst_element_set_state (engine->pipeline, target) == GST_STATE_CHANGE_FAILURE)
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_STREAM,
                   "Could not start the audio pipeline. Is an audio device "
                   "available?");
      teardown (engine);
      return FALSE;
    }
  return TRUE;
}

static gboolean
check_idle (GjEngine *engine, GError **error)
{
  if (engine->state != GJ_ENGINE_IDLE)
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_BUSY, "The engine is busy");
      return FALSE;
    }
  return TRUE;
}

/* ---------------------------------------------------------------------
 * Public transport API
 * ------------------------------------------------------------------- */

gboolean
gj_engine_play (GjEngine *engine, GjProject *project, gint64 start_ns,
                GError **error)
{
  GstElement *mixer = NULL, *tail;
  int n;

  if (!check_idle (engine, error))
    return FALSE;

  engine->pipeline = gst_pipeline_new ("garagejam-play");
  if (!(tail = add_mixer (engine->pipeline, project, &mixer, error)) ||
      (n = add_track_branches (engine->pipeline, mixer, project, error)) < 0 ||
      !add_playback_output (engine->pipeline, tail, error))
    {
      teardown (engine);
      return FALSE;
    }
  if (n == 0)
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_NOTHING_TO_PLAY,
                   "There are no audible tracks to play");
      teardown (engine);
      return FALSE;
    }

  engine->start_ns = MAX (start_ns, 0);
  return start_pipeline (engine, GJ_ENGINE_PLAYING, GST_STATE_PAUSED, error);
}

gboolean
gj_engine_record (GjEngine *engine, GjProject *project, const char *take_path,
                  GError **error)
{
  GstElement *src, *conv, *res, *caps, *level, *queue, *enc, *sink;
  g_autoptr (GstCaps) c = raw_caps (project, "S16LE");
  int audible = 0;

  if (!check_idle (engine, error))
    return FALSE;

  for (guint i = 0; i < project->tracks->len; i++)
    {
      GjTrack *t = g_ptr_array_index (project->tracks, i);
      if (t->path && g_file_test (t->path, G_FILE_TEST_IS_REGULAR))
        audible++;
    }

  engine->pipeline = gst_pipeline_new ("garagejam-record");

  /* Overdub: play the other tracks while recording the new take. */
  if (audible > 0)
    {
      GstElement *mixer = NULL, *tail;
      if (!(tail = add_mixer (engine->pipeline, project, &mixer, error)) ||
          add_track_branches (engine->pipeline, mixer, project, error) < 0 ||
          !add_playback_output (engine->pipeline, tail, error))
        {
          teardown (engine);
          return FALSE;
        }
    }

  if (!(src = make (engine->pipeline, "autoaudiosrc", NULL, error)) ||
      !(conv = make (engine->pipeline, "audioconvert", NULL, error)) ||
      !(res = make (engine->pipeline, "audioresample", NULL, error)) ||
      !(caps = make (engine->pipeline, "capsfilter", NULL, error)) ||
      !(level = make (engine->pipeline, "level", "inlevel", error)) ||
      !(queue = make (engine->pipeline, "queue", NULL, error)) ||
      !(enc = make (engine->pipeline, "wavenc", NULL, error)) ||
      !(sink = make (engine->pipeline, "filesink", NULL, error)))
    {
      teardown (engine);
      return FALSE;
    }

  g_object_set (caps, "caps", c, NULL);
  g_object_set (level, "interval", (guint64) 50 * GST_MSECOND,
                "post-messages", TRUE, NULL);
  g_object_set (sink, "location", take_path, NULL);

  if (!gst_element_link_many (src, conv, res, caps, level, queue, enc, sink, NULL))
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_STREAM,
                   "Could not build the recording chain");
      teardown (engine);
      return FALSE;
    }

  g_free (engine->record_path);
  engine->record_path = g_strdup (take_path);
  return start_pipeline (engine, GJ_ENGINE_RECORDING, GST_STATE_PLAYING, error);
}

gboolean
gj_engine_export (GjEngine *engine, GjProject *project, const char *wav_path,
                  GError **error)
{
  GstElement *mixer = NULL, *tail;
  int n;

  if (!check_idle (engine, error))
    return FALSE;

  engine->pipeline = gst_pipeline_new ("garagejam-export");
  if (!(tail = add_mixer (engine->pipeline, project, &mixer, error)) ||
      (n = add_track_branches (engine->pipeline, mixer, project, error)) < 0 ||
      !add_wav_output (engine->pipeline, tail, project, wav_path, error))
    {
      teardown (engine);
      return FALSE;
    }
  if (n == 0)
    {
      g_set_error (error, GJ_ERROR, GJ_ERROR_NOTHING_TO_PLAY,
                   "There are no audible tracks to export");
      teardown (engine);
      return FALSE;
    }

  g_free (engine->export_path);
  engine->export_path = g_strdup (wav_path);
  return start_pipeline (engine, GJ_ENGINE_EXPORTING, GST_STATE_PLAYING, error);
}

static gboolean
stop_timeout_cb (gpointer user_data)
{
  GjEngine *engine = user_data;

  engine->stop_timeout = 0;
  g_warning ("GarageJam: pipeline did not drain in time, forcing stop");
  finish (engine, NULL);
  return G_SOURCE_REMOVE;
}

void
gj_engine_stop (GjEngine *engine)
{
  switch (engine->state)
    {
    case GJ_ENGINE_RECORDING:
      /* Send EOS so wavenc can rewrite the RIFF header with final sizes. */
      engine->state = GJ_ENGINE_STOPPING;
      gst_element_send_event (engine->pipeline, gst_event_new_eos ());
      engine->stop_timeout = g_timeout_add_seconds (3, stop_timeout_cb, engine);
      break;

    case GJ_ENGINE_PLAYING:
      finish (engine, NULL);
      break;

    case GJ_ENGINE_EXPORTING:
      {
        g_autoptr (GError) err = g_error_new_literal (G_IO_ERROR,
                                                      G_IO_ERROR_CANCELLED,
                                                      "Export cancelled");
        finish (engine, err);
      }
      break;

    case GJ_ENGINE_STOPPING:
    case GJ_ENGINE_IDLE:
    default:
      break;
    }
}
