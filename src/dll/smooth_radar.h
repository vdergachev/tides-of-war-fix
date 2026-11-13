#ifndef TOW_SMOOTH_RADAR_H
#define TOW_SMOOTH_RADAR_H
#include "scene.h"
#ifdef __cplusplus
extern "C" {
#endif
void smooth_radar_configure(const char *path);
void smooth_radar_install(void);
void smooth_radar_begin(scene_record *record, BYTE *frame);
void smooth_radar_finish(LONG epoch);
void smooth_radar_reset(void);
void smooth_radar_log(void);
#ifdef __cplusplus
}
#endif
#endif
