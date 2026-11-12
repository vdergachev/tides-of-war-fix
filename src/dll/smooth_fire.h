#ifndef TOW_SMOOTH_FIRE_H
#define TOW_SMOOTH_FIRE_H
#include "scene.h"
#ifdef __cplusplus
extern "C" {
#endif
void smooth_fire_configure(const char *path);
int smooth_fire_enabled(void);
void smooth_fire_install(void);
void smooth_fire_begin(scene_record *record, BYTE *frame);
void smooth_fire_objects_done(void);
void smooth_fire_sprite(const DWORD *saved);
void smooth_fire_finish(void);
void smooth_fire_log(void);
#ifdef __cplusplus
}
#endif
#endif
