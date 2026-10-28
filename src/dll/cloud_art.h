#ifndef TOW_CLOUD_ART_H
#define TOW_CLOUD_ART_H
#include <windows.h>
#define CLOUD_IMAGES 36

typedef struct {
    unsigned width;
    unsigned height;
    DWORD *pixels;
} cloud_art;

/* Reads Bag2 beside EXE; caller frees pixels with free(). No game writes. */
#ifdef __cplusplus
extern "C" {
#endif

void cloud_art_load(cloud_art images[CLOUD_IMAGES]);
#ifdef __cplusplus
}
#endif

#endif
