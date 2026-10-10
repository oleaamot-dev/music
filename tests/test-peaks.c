/* test-peaks.c - WAV peak reader tests
 * Copyright (C) 2026 Ole Kristian Aamot
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <unistd.h>
#include <string.h>
#include <glib/gstdio.h>
#include "gj-waveform.h"

static void
put32 (GByteArray *b, guint32 v)
{
  guint8 x[4] = { v, v >> 8, v >> 16, v >> 24 };
  g_byte_array_append (b, x, 4);
}

static void
put16 (GByteArray *b, guint16 v)
{
  guint8 x[2] = { v, v >> 8 };
  g_byte_array_append (b, x, 2);
}

/* one second of stereo 48 kHz: left = half-scale sine, right = silence;
 * with a LIST chunk before "data" to exercise chunk skipping */
static char *
write_wav (gboolean zero_size)
{
  g_autoptr (GByteArray) b = g_byte_array_new ();
  const guint rate = 48000, frames = 48000;
  char *path = NULL;
  int fd = g_file_open_tmp ("gj-peaks-XXXXXX.wav", &path, NULL);

  close (fd);
  g_byte_array_append (b, (guint8 *) "RIFF", 4);
  put32 (b, 0);
  g_byte_array_append (b, (guint8 *) "WAVEfmt ", 8);
  put32 (b, 16);
  put16 (b, 1); put16 (b, 2); put32 (b, rate); put32 (b, rate * 4);
  put16 (b, 4); put16 (b, 16);
  g_byte_array_append (b, (guint8 *) "LIST", 4);
  put32 (b, 3);
  g_byte_array_append (b, (guint8 *) "abc\0", 4);   /* odd size + pad */
  g_byte_array_append (b, (guint8 *) "data", 4);
  put32 (b, zero_size ? 0 : frames * 4);
  for (guint i = 0; i < frames; i++)
    {
      put16 (b, (guint16) (gint16) (16384 * sin (2 * G_PI * 440 * i / rate)));
      put16 (b, 0);
    }
  g_file_set_contents (path, (char *) b->data, b->len, NULL);
  return path;
}

static void
check (gboolean zero_size)
{
  g_autofree char *path = write_wav (zero_size);
  g_autoptr (GError) error = NULL;
  GjPeaks *peaks = gj_peaks_read_wav (path, &error);

  g_assert_no_error (error);
  g_assert_nonnull (peaks);
  g_assert_cmpfloat_with_epsilon (peaks->duration, 1.0, 1e-6);
  g_assert_cmpuint (peaks->n_buckets, ==, GJ_PEAKS_PER_SECOND);
  for (gsize i = 0; i < peaks->n_buckets; i++)
    {
      g_assert_cmpfloat (peaks->max[i], >, 0.45);
      g_assert_cmpfloat (peaks->max[i], <=, 0.51);
      g_assert_cmpfloat (peaks->min[i], <, -0.45);
    }
  gj_peaks_free (peaks);
  g_unlink (path);
}

static void test_peaks (void) { check (FALSE); }
static void test_peaks_unfinished (void) { check (TRUE); }

static void
test_not_wav (void)
{
  g_autoptr (GError) error = NULL;
  char *path = NULL;
  int fd = g_file_open_tmp ("gj-peaks-XXXXXX.txt", &path, NULL);

  close (fd);
  g_file_set_contents (path, "this is not audio", -1, NULL);
  g_assert_null (gj_peaks_read_wav (path, &error));
  g_assert_nonnull (error);
  g_unlink (path);
  g_free (path);
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/peaks/wav", test_peaks);
  g_test_add_func ("/peaks/unfinished-header", test_peaks_unfinished);
  g_test_add_func ("/peaks/not-wav", test_not_wav);
  return g_test_run ();
}
