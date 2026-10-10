/* gj-engine.h - GarageJam GStreamer audio engine
 *
 * Copyright (C) 2026 Ole Kristian Aamot <ole.aamot@aamotinnovation.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GJ_ENGINE_H
#define GJ_ENGINE_H

#include <gst/gst.h>
#include "gj-project.h"

G_BEGIN_DECLS

typedef enum {
  GJ_ENGINE_IDLE,
  GJ_ENGINE_PLAYING,
  GJ_ENGINE_RECORDING,
  GJ_ENGINE_EXPORTING,
  GJ_ENGINE_STOPPING
} GjEngineState;

typedef enum {
  GJ_METER_INPUT,
  GJ_METER_OUTPUT
} GjMeter;

typedef struct _GjEngine GjEngine;

/* peak level in dBFS (-inf … 0) */
typedef void (*GjEngineLevelFunc)    (GjEngine *engine, GjMeter meter,
                                      double peak_db, gpointer user_data);
/* error is NULL on success; record_path is the finished take, or NULL */
typedef void (*GjEngineFinishedFunc) (GjEngine *engine, GjEngineState what,
                                      const GError *error,
                                      const char *record_path,
                                      gpointer user_data);

GjEngine      *gj_engine_new          (void);
void           gj_engine_free         (GjEngine *engine);
void           gj_engine_set_callbacks(GjEngine *engine,
                                       GjEngineLevelFunc level_func,
                                       GjEngineFinishedFunc finished_func,
                                       gpointer user_data);

gboolean       gj_engine_play         (GjEngine *engine, GjProject *project,
                                       gint64 start_ns, GError **error);
gboolean       gj_engine_record       (GjEngine *engine, GjProject *project,
                                       const char *take_path, GError **error);
gboolean       gj_engine_export       (GjEngine *engine, GjProject *project,
                                       const char *wav_path, GError **error);
void           gj_engine_stop         (GjEngine *engine);
void           gj_engine_update_mix   (GjEngine *engine, GjProject *project);

GjEngineState  gj_engine_get_state    (GjEngine *engine);
gint64         gj_engine_get_position (GjEngine *engine);

G_END_DECLS

#endif /* GJ_ENGINE_H */
