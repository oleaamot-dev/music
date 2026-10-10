/* gj-waveform.h - waveform peak reader and timeline drawing widget
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GJ_WAVEFORM_H
#define GJ_WAVEFORM_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GJ_PEAKS_PER_SECOND 100

typedef struct {
  float  *min;        /* per bucket, -1 … 1 */
  float  *max;
  gsize   n_buckets;
  double  duration;   /* seconds */
} GjPeaks;

/* Reads a 16-bit PCM RIFF/WAVE file. Returns NULL for other formats. */
GjPeaks *gj_peaks_read_wav (const char *path, GError **error);
void     gj_peaks_free     (GjPeaks *peaks);

#define GJ_TYPE_WAVEFORM (gj_waveform_get_type ())
G_DECLARE_FINAL_TYPE (GjWaveform, gj_waveform, GJ, WAVEFORM, GtkWidget)

GtkWidget *gj_waveform_new          (void);
void       gj_waveform_set_file     (GjWaveform *self, const char *path);
double     gj_waveform_get_duration (GjWaveform *self);
void       gj_waveform_set_timeline (GjWaveform *self, double length_seconds,
                                     double playhead_seconds);
void       gj_waveform_set_recording(GjWaveform *self, gboolean recording);
void       gj_waveform_set_dimmed   (GjWaveform *self, gboolean dimmed);

G_END_DECLS

#endif /* GJ_WAVEFORM_H */
