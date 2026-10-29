#ifndef TOW_SHIP_LIVE_H
#define TOW_SHIP_LIVE_H
#include "ship_dump.h"

/* Bounded scene mailbox payload. No pointers into the game survive publication. */
#define HD_LIVE_PASSES   32u
#define HD_LIVE_DRAWS    1024u
#define HD_LIVE_TEXTURES 64u
#define HD_LIVE_GEOMETRY (2u * 1024u * 1024u)
#define HD_LIVE_PIXELS   (4u * 1024u * 1024u)

typedef struct {
    unsigned passes;
    unsigned draws;
    unsigned textures;
    unsigned geometry_bytes;
    unsigned pixel_bytes;
    double capture_ms;
    hd_pass pass[HD_LIVE_PASSES];
    hd_draw draw[HD_LIVE_DRAWS];
    hd_texture texture[HD_LIVE_TEXTURES];
    unsigned offsets[HD_LIVE_TEXTURES];
    unsigned char geometry[HD_LIVE_GEOMETRY];
    unsigned char pixels[HD_LIVE_PIXELS];
} hd_live_record;

#ifdef __cplusplus
extern "C" {
#endif
int ship_hd_render_scale(void);
void ship_capture_publish(hd_live_record *out);
#ifdef __cplusplus
}
#endif
#endif
