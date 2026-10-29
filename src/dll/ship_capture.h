#ifndef TOW_SHIP_CAPTURE_H
#define TOW_SHIP_CAPTURE_H

#include <windows.h>
#include "ship_dump.h"

/* All callbacks run synchronously on the game thread. */
void ship_capture_configure(const char *ini);
void ship_capture_apply(const char *text_hash);
int ship_capture_enabled(void);
int ship_capture_identity(const void *image, hd_pass *out);
void ship_capture_draw(void *device, unsigned primitive, unsigned vtype, const void *vertices, unsigned count,
                       const WORD *indices, unsigned index_count, unsigned flags, HRESULT result);

#endif
