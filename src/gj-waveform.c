/* gj-waveform.c - waveform peak reader and timeline drawing widget
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>

#include "gj-project.h"
#include "gj-waveform.h"

/* ---------------------------------------------------------------------
 * WAV peak reader
 * ------------------------------------------------------------------- */

static guint32
le32 (const guint8 *p)
{
  return (guint32) p[0] | ((guint32) p[1] << 8) | ((guint32) p[2] << 16) |
         ((guint32) p[3] << 24);
}

static guint16
le16 (const guint8 *p)
{
  return (guint16) (p[0] | (p[1] << 8));
}

void
gj_peaks_free (GjPeaks *peaks)
{
  if (peaks == NULL)
    return;
  g_free (peaks->min);
  g_free (peaks->max);
  g_free (peaks);
}

GjPeaks *
gj_peaks_read_wav (const char *path, GError **error)
{
  FILE *f;
  guint8 hdr[12], ch[8], fmt[16];
  guint16 channels = 0, bits = 0, format = 0;
  guint32 rate = 0, data_size = 0;
  gboolean have_fmt = FALSE, have_data = FALSE;
  GjPeaks *peaks = NULL;

  f = g_fopen (path, "rb");
  if (f == NULL)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Cannot open %s", path);
      return NULL;
    }

  if (fread (hdr, 1, 12, f) != 12 || memcmp (hdr, "RIFF", 4) != 0 ||
      memcmp (hdr + 8, "WAVE", 4) != 0)
    goto unsupported;

  while (!have_data && fread (ch, 1, 8, f) == 8)
    {
      guint32 size = le32 (ch + 4);

      if (memcmp (ch, "fmt ", 4) == 0)
        {
          if (size < 16 || fread (fmt, 1, 16, f) != 16)
            goto unsupported;
          format = le16 (fmt);
          channels = le16 (fmt + 2);
          rate = le32 (fmt + 4);
          bits = le16 (fmt + 14);
          have_fmt = TRUE;
          if (fseek (f, (long) (size - 16 + (size & 1)), SEEK_CUR) != 0)
            goto unsupported;
        }
      else if (memcmp (ch, "data", 4) == 0)
        {
          data_size = size;
          have_data = TRUE;
        }
      else if (fseek (f, (long) (size + (size & 1)), SEEK_CUR) != 0)
        goto unsupported;
    }

  /* WAVE_FORMAT_PCM or WAVE_FORMAT_EXTENSIBLE carrying 16-bit PCM */
  if (!have_fmt || !have_data || (format != 1 && format != 0xFFFE) ||
      bits != 16 || channels == 0 || rate == 0)
    goto unsupported;

  {
    gsize frames_per_bucket = MAX (1, rate / GJ_PEAKS_PER_SECOND);
    gsize frame_bytes = (gsize) channels * 2;
    gsize capacity = 1024, n = 0, frame_in_bucket = 0;
    guint64 remaining = (data_size == 0 || data_size == 0xFFFFFFFF)
                        ? G_MAXUINT64 : data_size;
    gsize total_frames = 0;
    float cur_min = 0, cur_max = 0;
    guint8 buf[16384];

    peaks = g_new0 (GjPeaks, 1);
    peaks->min = g_new (float, capacity);
    peaks->max = g_new (float, capacity);

    while (remaining > 0)
      {
        gsize want = (gsize) MIN ((guint64) sizeof buf - sizeof buf % frame_bytes,
                                  remaining);
        gsize got = fread (buf, 1, want, f);
        got -= got % frame_bytes;
        if (got == 0)
          break;
        remaining -= got;

        for (gsize off = 0; off < got; off += frame_bytes)
          {
            for (guint c = 0; c < channels; c++)
              {
                gint16 s = (gint16) le16 (buf + off + c * 2);
                float v = s / 32768.0f;
                cur_min = MIN (cur_min, v);
                cur_max = MAX (cur_max, v);
              }
            total_frames++;
            if (++frame_in_bucket == frames_per_bucket)
              {
                if (n == capacity)
                  {
                    capacity *= 2;
                    peaks->min = g_renew (float, peaks->min, capacity);
                    peaks->max = g_renew (float, peaks->max, capacity);
                  }
                peaks->min[n] = cur_min;
                peaks->max[n] = cur_max;
                n++;
                frame_in_bucket = 0;
                cur_min = cur_max = 0;
              }
          }
      }

    peaks->n_buckets = n;
    peaks->duration = (double) total_frames / rate;
  }

  fclose (f);
  return peaks;

unsupported:
  fclose (f);
  g_set_error (error, GJ_ERROR, GJ_ERROR_PARSE,
               "%s is not a 16-bit PCM WAV file", path);
  return NULL;
}

/* ---------------------------------------------------------------------
 * GjWaveform widget
 * ------------------------------------------------------------------- */

struct _GjWaveform {
  GtkWidget  parent_instance;
  GjPeaks   *peaks;
  double     length;     /* seconds represented by the full width */
  double     playhead;   /* seconds, < 0 hides */
  gboolean   recording;
  gboolean   dimmed;
};

G_DEFINE_FINAL_TYPE (GjWaveform, gj_waveform, GTK_TYPE_WIDGET)

enum { SIGNAL_SEEK, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
draw (GjWaveform *self, cairo_t *cr, int width, int height)
{
  GdkRGBA fg;
  double mid = height / 2.0;

  gtk_widget_get_color (GTK_WIDGET (self), &fg);

  /* background and centre line */
  cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.06);
  cairo_rectangle (cr, 0, 0, width, height);
  cairo_fill (cr);

  /* bar grid: one faint line per 5 seconds */
  cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.10);
  cairo_set_line_width (cr, 1);
  for (double t = 5; t < self->length; t += 5)
    {
      double x = floor (t / self->length * width) + 0.5;
      cairo_move_to (cr, x, 0);
      cairo_line_to (cr, x, height);
    }
  cairo_stroke (cr);

  if (self->peaks && self->peaks->n_buckets > 0)
    {
      double buckets_per_px = self->length * GJ_PEAKS_PER_SECOND / width;

      if (self->recording)
        cairo_set_source_rgba (cr, 0.88, 0.11, 0.14, 0.85);
      else if (self->dimmed)
        cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.25);
      else
        cairo_set_source_rgba (cr, 0.21, 0.52, 0.89, 0.90);

      for (int x = 0; x < width; x++)
        {
          gsize b0 = (gsize) (x * buckets_per_px);
          gsize b1 = (gsize) ((x + 1) * buckets_per_px);
          float lo = 0, hi = 0;

          if (b0 >= self->peaks->n_buckets)
            break;
          b1 = CLAMP (b1, b0 + 1, self->peaks->n_buckets);
          for (gsize b = b0; b < b1; b++)
            {
              lo = MIN (lo, self->peaks->min[b]);
              hi = MAX (hi, self->peaks->max[b]);
            }
          cairo_rectangle (cr, x, mid - hi * mid, 1, MAX (1.0, (hi - lo) * mid));
        }
      cairo_fill (cr);
    }
  else
    {
      cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.25);
      cairo_move_to (cr, 0, mid + 0.5);
      cairo_line_to (cr, width, mid + 0.5);
      cairo_stroke (cr);
    }

  if (self->playhead >= 0 && self->length > 0)
    {
      double x = floor (self->playhead / self->length * width) + 0.5;
      cairo_set_source_rgba (cr, 0.88, 0.11, 0.14, 1.0);
      cairo_set_line_width (cr, 2);
      cairo_move_to (cr, x, 0);
      cairo_line_to (cr, x, height);
      cairo_stroke (cr);
    }
}

static void
gj_waveform_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  GjWaveform *self = GJ_WAVEFORM (widget);
  int width = gtk_widget_get_width (widget);
  int height = gtk_widget_get_height (widget);
  cairo_t *cr;

  if (width <= 0 || height <= 0)
    return;
  cr = gtk_snapshot_append_cairo (snapshot,
                                  &GRAPHENE_RECT_INIT (0, 0, width, height));
  draw (self, cr, width, height);
  cairo_destroy (cr);
}

static void
on_click (GtkGestureClick *gesture, int n_press, double x, double y,
          GjWaveform *self)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self));

  if (width > 0)
    g_signal_emit (self, signals[SIGNAL_SEEK], 0,
                   CLAMP (x / width, 0.0, 1.0) * self->length);
}

static void
gj_waveform_finalize (GObject *object)
{
  gj_peaks_free (GJ_WAVEFORM (object)->peaks);
  G_OBJECT_CLASS (gj_waveform_parent_class)->finalize (object);
}

static void
gj_waveform_class_init (GjWaveformClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gj_waveform_finalize;
  widget_class->snapshot = gj_waveform_snapshot;
  gtk_widget_class_set_css_name (widget_class, "gj-waveform");

  /* GjWaveform::seek (double seconds) */
  signals[SIGNAL_SEEK] = g_signal_new ("seek", G_TYPE_FROM_CLASS (klass),
                                       G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 1, G_TYPE_DOUBLE);
}

static void
gj_waveform_init (GjWaveform *self)
{
  GtkGesture *click = gtk_gesture_click_new ();

  self->length = 30.0;
  self->playhead = 0.0;
  g_signal_connect (click, "pressed", G_CALLBACK (on_click), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));
  gtk_widget_set_size_request (GTK_WIDGET (self), 200, 88);
  gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
  gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "text");
  gtk_widget_set_overflow (GTK_WIDGET (self), GTK_OVERFLOW_HIDDEN);
}

GtkWidget *
gj_waveform_new (void)
{
  return g_object_new (GJ_TYPE_WAVEFORM, NULL);
}

void
gj_waveform_set_file (GjWaveform *self, const char *path)
{
  g_clear_pointer (&self->peaks, gj_peaks_free);
  if (path && g_file_test (path, G_FILE_TEST_IS_REGULAR))
    self->peaks = gj_peaks_read_wav (path, NULL);
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

double
gj_waveform_get_duration (GjWaveform *self)
{
  return self->peaks ? self->peaks->duration : 0.0;
}

void
gj_waveform_set_timeline (GjWaveform *self, double length_seconds,
                          double playhead_seconds)
{
  if (self->length == length_seconds && self->playhead == playhead_seconds)
    return;
  self->length = MAX (length_seconds, 1.0);
  self->playhead = playhead_seconds;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
gj_waveform_set_recording (GjWaveform *self, gboolean recording)
{
  self->recording = recording;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
gj_waveform_set_dimmed (GjWaveform *self, gboolean dimmed)
{
  if (self->dimmed == dimmed)
    return;
  self->dimmed = dimmed;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}
