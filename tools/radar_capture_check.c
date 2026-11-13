/* Exercise guarded capture and the native bridge using only owned fixtures. */
#include <windows.h>
static BYTE *fixture_frame;
static BYTE *fixture_table;
static BYTE *fixture_radar;
static int fixture_width = 100000;
static int fixture_height = 100000;
static RECT fixture_rect = {0, 0, 128, 128};
static int fixture_tiles = 42;
static double fixture_radius = 48;
static POINT fixture_centre = {64, 64};
static BYTE fixture_night;
#define POINTER_RADIUS_ADDRESS ((uintptr_t)&fixture_radius)
#define POINTER_CENTRE_ADDRESS ((uintptr_t)&fixture_centre)
#define NIGHT_ADDRESS          ((uintptr_t)&fixture_night)
#define FRAME_ADDRESS          ((uintptr_t)&fixture_frame)
#define TABLE_ADDRESS          ((uintptr_t)&fixture_table)
#define RADAR_ADDRESS          ((uintptr_t)&fixture_radar)
#define WORLD_WIDTH            ((uintptr_t)&fixture_width)
#define WORLD_HEIGHT           ((uintptr_t)&fixture_height)
#define RADAR_RECT             ((uintptr_t)&fixture_rect)
#define TILE_COUNT             ((uintptr_t)&fixture_tiles)
#include "../src/dll/smooth_radar.c"
#include <stdio.h>

#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                                     \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

/* No patching or logging in this fixture. */
int patch_check(void *p, const uint8_t *bytes, size_t count)
{
    (void)p;
    (void)bytes;
    (void)count;
    return 0;
}

int patch_call(void *p, const uint8_t *bytes, void *target)
{
    (void)p;
    (void)bytes;
    (void)target;
    return 0;
}

void log_line(const char *format, ...)
{
    (void)format;
}

void diag_log(const char *format, ...)
{
    (void)format;
}

static BYTE frame[0x68];
static WORD frame_bits[SCENE_PIXELS];
static BYTE ship[0x100];
static BYTE table[0xa48];
static BYTE radar[0x100];
static BYTE *objects[1] = {ship};
static double pose[4] = {2367, 2367, 0, 0};
static BYTE dib[0x68];
static WORD art[8 * 8];
static DWORD sprite[32];
static WORD wind[65 * 13];
static scene_record record;
static DWORD mock_calls;
static DWORD mock_this;
static DWORD mock_arg;

// clang-format off
/* One-argument thiscall fixture returns a known EAX/EDX/CF/LastError. */
__declspec(naked) static void mock_terrain(void)
{
    __asm {
        inc mock_calls
        mov mock_this, ecx
        mov eax, [esp + 4]
        mov mock_arg, eax
        mov eax, 12345678h
        mov edx, 23456789h
        stc
        ret 4
    }
}

/* Match CrowsNest_Draw's three-argument thiscall. */
__declspec(naked) static void mock_radar(void)
{
    __asm {
        inc mock_calls
        mov mock_this, ecx
        mov eax, [esp + 4]
        add eax, [esp + 8]
        add eax, [esp + 12]
        mov mock_arg, eax
        mov eax, 12345678h
        mov edx, 23456789h
        stc
        ret 12
    }
}

// clang-format on

static void check_radar_bridge(void)
{
    g_radar_fn = (DWORD)(uintptr_t)mock_radar;
    DWORD stack0;
    DWORD stack1;
    DWORD result;
    DWORD result_edx;
    BYTE carry;
    SetLastError(0x1357);
    // clang-format off
    __asm {
        mov stack0, esp
        lea ecx, radar
        push 3
        push 2
        push 1
        call radar_thunk
        mov stack1, esp
        mov result, eax
        mov result_edx, edx
        setc carry
    }
    // clang-format on
    CHECK(stack0 == stack1 && result == 0x12345678 && result_edx == 0x23456789 && carry);
    CHECK(mock_this == (DWORD)radar && mock_arg == 6);
    CHECK(GetLastError() == 0x1357);
}

static void setup(void)
{
    *(int *)(frame + DIB_WIDTH) = SCENE_WIDTH;
    *(int *)(frame + DIB_HEIGHT) = SCENE_HEIGHT;
    *(WORD *)(frame + DIB_BPP) = 16;
    *(WORD **)(frame + DIB_BITS) = frame_bits;
    *(int *)(frame + DIB_PITCH) = SCENE_WIDTH * 2;
    *(int *)(dib + DIB_WIDTH) = 8;
    *(int *)(dib + DIB_HEIGHT) = 8;
    *(WORD *)(dib + DIB_BPP) = 16;
    *(WORD **)(dib + DIB_BITS) = art;
    *(int *)(dib + DIB_PITCH) = 16;
    for (unsigned i = 0; i < SCENE_PIXELS; ++i) {
        frame_bits[i] = 0x001f;
    }
    for (unsigned i = 0; i < 64; ++i) {
        art[i] = i ? 0xffe0 : 0x07e0;
    }
    g_enabled = 1;
    g_ready = 1;
    record.valid = 1;
    fixture_frame = frame;
    fixture_table = table;
    fixture_radar = radar;
    *(BYTE ***)(table + TABLE_ARRAY) = objects;
    *(unsigned *)(table + TABLE_COUNT) = 1;
    *(double **)(ship + SHIP_POSE) = pose;
    *(POINT *)(ship + SHIP_POINT) = (POINT){2367, 2367};
    *(RECT *)(radar + RADAR_VIEW + CAMERA_RECT) = (RECT){1023, 1023, 3711, 3711};
    *(POINT *)(radar + RADAR_VIEW + CAMERA_CENTRE) = (POINT){2367, 2367};
}

static void begin(void)
{
    smooth_radar_begin(&record, frame);
    DWORD radar_saved[12] = {0};
    radar_saved[11] = (DWORD)frame;
    radar_enter(radar_saved);
    /* Keep terrain observers inert: the fixture has no game's fixed globals. */
    g_complete = 0;
    g_terrain_fn = (DWORD)(uintptr_t)mock_terrain;
    DWORD stack0;
    DWORD stack1;
    DWORD result;
    DWORD result_edx;
    BYTE carry;
    SetLastError(0x1357);
    // clang-format off
    __asm {
        mov stack0, esp
        lea ecx, ship
        lea eax, frame
        push eax
        call terrain_thunk
        mov stack1, esp
        mov result, eax
        mov result_edx, edx
        setc carry
    }
    // clang-format on
    CHECK(stack0 == stack1 && result == 0x12345678 && result_edx == 0x23456789 && carry);
    CHECK(mock_calls && mock_this == (DWORD)ship && mock_arg == (DWORD)frame);
    CHECK(GetLastError() == 0x1357);
    g_complete = 1;
    g_terrain = 1;
    CHECK(viewport(&g_capture, radar));
    for (unsigned i = 0; i < RADAR_PIXELS; ++i) {
        g_capture.terrain[i] = 0x001f;
        g_replay[i] = 0x001f;
    }
}

static void blit(unsigned site, DWORD *saved, int x, int y)
{
    static BYTE red = 0;
    static BYTE green = 255;
    static BYTE blue = 0;
    int size = 8;
    int source = 0;
    saved[6] = (DWORD)dib;
    saved[10] = site + 5;
    saved[11] = (DWORD)frame;
    saved[12] = (DWORD)&red;
    saved[13] = (DWORD)&green;
    saved[14] = (DWORD)&blue;
    saved[15] = (DWORD)&size;
    saved[16] = (DWORD)&size;
    saved[17] = (DWORD)&source;
    saved[18] = (DWORD)&source;
    saved[19] = (DWORD)&x;
    saved[20] = (DWORD)&y;
    blit_enter(saved);
}

static void finish_pixels(void)
{
    for (unsigned y = 0; y < RADAR_SIDE; ++y) {
        memcpy(record.pixels[SCENE_C] + y * SCENE_WIDTH, g_replay + y * RADAR_SIDE, RADAR_SIDE * 2);
        memcpy(frame_bits + y * SCENE_WIDTH, g_replay + y * RADAR_SIDE, RADAR_SIDE * 2);
    }
}

static void check_wind(int x, int y)
{
    BYTE *image = (BYTE *)sprite + 4;
    *(int *)(image + DIB_WIDTH) = 65;
    *(int *)(image + DIB_HEIGHT) = 13;
    *(WORD *)(image + DIB_BPP) = 16;
    *(WORD **)(image + DIB_BITS) = wind;
    *(int *)(image + DIB_PITCH) = 130;
    sprite[27] = 13;
    sprite[29] = 1;
    sprite[30] = 5;
    for (unsigned i = 0; i < 65 * 13; ++i) {
        wind[i] = 0x07e0;
    }
    wind[6 * 65 + 2 * 13 + 6] = 0xf800;
    DWORD saved[22] = {0};
    unsigned index = 2;
    BYTE red = 0;
    BYTE green = 255;
    BYTE blue = 0;
    int size = 13;
    int source = 0;
    saved[6] = (DWORD)sprite;
    saved[10] = WIND_CALL + 5;
    saved[11] = (DWORD)&index;
    saved[12] = (DWORD)frame;
    saved[13] = (DWORD)&red;
    saved[14] = (DWORD)&green;
    saved[15] = (DWORD)&blue;
    saved[16] = (DWORD)&size;
    saved[17] = (DWORD)&size;
    saved[18] = (DWORD)&source;
    saved[19] = (DWORD)&source;
    saved[20] = (DWORD)&x;
    saved[21] = (DWORD)&y;
    wind_enter(saved);
    CHECK(g_capture.pointers[RADAR_WIND].present);
    CHECK(g_capture.pointers[RADAR_WIND].frame == 2);
    CHECK((g_capture.overlay_mask[(y + 6) * RADAR_SIDE + x + 6] & RADAR_WIND_MASK) ==
          (g_capture.pointers[RADAR_WIND].valid ? 0 : RADAR_WIND_MASK));
    CHECK(g_replay[(y + 6) * RADAR_SIDE + x + 6] == 0xf800);
}

static void check_reuse(void)
{
    smooth_radar_finish(1);
    CHECK(record.radar.valid && g_last.valid);
    /* Add one already admitted mobile to test per-step scene pose refresh. */
    g_last.markers[1] = g_last.markers[0];
    g_last.markers[1].kind = 0;
    g_last.markers[1].id = 0;
    g_last.count = 2;
    record.count = 1;
    record.ships[0].ptr = (uintptr_t)ship;
    record.ships[0].id = 0;
    record.ships[0].x = 2370.25;
    record.ships[0].y = 2368.5;
    pose[0] = 2370.25;
    pose[1] = 2368.5;
    *(POINT *)(ship + SHIP_POINT) = (POINT){2370, 2369};
    for (unsigned partial = 0; partial < 3; ++partial) {
        smooth_radar_begin(&record, frame);
        if (partial) {
            DWORD saved[12] = {0};
            saved[11] = (DWORD)frame;
            radar_enter(saved);
            g_terrain = partial == 1;
            g_compass_calls = partial == 2 ? 4 : 0;
            g_capture.terrain[123] = 0xdead;
            frame_bits[123] ^= 1;
            record.pixels[SCENE_C][123] ^= 1;
            radar_leave(NULL);
        }
        record.count = partial ? 0 : 1;
        smooth_radar_finish(1);
        CHECK(record.radar.valid && record.radar.failure == RADAR_OK);
        CHECK(record.radar.markers[1].x == 2370.25 && record.radar.markers[1].y == 2368.5);
        CHECK(record.radar.markers[0].x == 2367); // port retains its world position
        CHECK(record.radar.terrain[123] != 0xdead);
    }
    CHECK(g_reused == 3 && g_partial == 2);
    smooth_radar_begin(&record, frame);
    record.pixels[SCENE_C][0] ^= 1;
    smooth_radar_finish(1);
    CHECK(record.radar.valid && record.radar.failure == RADAR_OK);
    CHECK(record.radar.overlay_mask[0] && record.radar.final[0] == record.pixels[SCENE_C][0]);
    CHECK(g_last.final[0] != record.radar.final[0]);
    record.pixels[SCENE_C][0] ^= 1;
    smooth_radar_begin(&record, frame);
    DWORD saved[12] = {0};
    saved[11] = (DWORD)frame;
    radar_enter(saved);
    radar_leave(NULL);
    record.pixels[SCENE_C][0] ^= 1;
    smooth_radar_finish(1);
    CHECK(record.radar.valid && record.radar.failure == RADAR_OK);
    CHECK(record.radar.overlay_mask[0] && record.radar.final[0] == record.pixels[SCENE_C][0]);
    CHECK(g_last.final[0] != record.radar.final[0]);
    record.pixels[SCENE_C][0] ^= 1;
    smooth_radar_begin(&record, frame);
    smooth_radar_finish(2);
    CHECK(!record.radar.valid && record.radar.failure == RADAR_INCOMPLETE);
    smooth_radar_reset();
    CHECK(!g_last.valid && !g_record);
    puts("radar capture: skipped/terrain-only/compass-only reuse, pose refresh, UI/epoch reset OK");
    pose[0] = pose[1] = 2367;
    *(POINT *)(ship + SHIP_POINT) = (POINT){2367, 2367};
}

static void check_optional_pose(void)
{
    begin();
    DWORD saved[25] = {0};
    POINT world = {2367, 2367};
    POINT projected = {64, 64};
    RECT clip = {0, 0, 128, 128};
    saved[1] = (DWORD)ship;
    saved[10] = MOBILE_PROJECT_CALL + 5;
    saved[11] = (DWORD)&world;
    *(double **)(ship + SHIP_POSE) = (double *)1;
    LONG unavailable = g_pose_unavailable;
    project_enter(saved);
    CHECK(g_complete && g_has_pending && g_pose_unavailable == unavailable);
    saved[10] = MOBILE_CALL + 5;
    saved[11] = (DWORD)frame;
    saved[12] = (DWORD)&clip;
    saved[13] = (DWORD)&projected;
    marker_enter(saved);
    CHECK(g_complete && g_has_admitted && g_pose_unavailable == unavailable + 1);
    CHECK(g_admitted.x == world.x && g_admitted.y == world.y);
    blit(MOBILE_BLIT, saved, 60, 60);
    CHECK(g_complete && g_capture.count == 1);
    *(double **)(ship + SHIP_POSE) = pose;
    /* Wrong-phase pose must use the game's POINT, not another simulation step. */
    pose[0] += 100;
    mobile_position(&g_admitted, world);
    CHECK(g_admitted.x == world.x);
    pose[0] = world.x + .25;
    mobile_position(&g_admitted, world);
    CHECK(g_admitted.x == world.x + .25);
    /* No 3D ship is present. The admitted radar identity still moves. */
    record.count = 0;
    refresh_mobile(&g_capture.markers[0]);
    CHECK(g_capture.markers[0].x == world.x + .25);
    ++g_capture.markers[0].id;
    refresh_mobile(&g_capture.markers[0]);
    CHECK(g_capture.markers[0].x == world.x + .25);
    pose[0] = world.x;
    puts("radar capture: hidden/invalid pose, phase mismatch, offscreen mobile, ptr+ID OK");
}

static BYTE frame_art[0x68];
static WORD frame_art_bits[RADAR_PIXELS];

static void frame_piece(DWORD site, int width, int height, int sx, int sy, int dx, int dy)
{
    DWORD saved[21] = {0};
    BYTE red = 0;
    BYTE green = 255;
    BYTE blue = 0;
    saved[6] = (DWORD)frame_art;
    saved[10] = site + 5;
    saved[11] = (DWORD)frame;
    saved[12] = (DWORD)&red;
    saved[13] = (DWORD)&green;
    saved[14] = (DWORD)&blue;
    saved[15] = (DWORD)&width;
    saved[16] = (DWORD)&height;
    saved[17] = (DWORD)&sx;
    saved[18] = (DWORD)&sy;
    saved[19] = (DWORD)&dx;
    saved[20] = (DWORD)&dy;
    blit_enter(saved);
    CHECK(g_complete);
}

static void check_frame_variants(void)
{
    static const RECT pieces[8] = {{0, 0, 32, 32},     {32, 0, 96, 20},    {96, 0, 128, 32},
                                   {0, 32, 20, 96},    {108, 32, 128, 96}, {0, 96, 32, 128},
                                   {32, 108, 96, 128}, {96, 96, 128, 128}};
    *(WORD *)(frame_art + DIB_BPP) = 16;
    for (unsigned variant = 0; variant < 6; ++variant) {
        begin();
        DWORD saved[21] = {0};
        for (unsigned i = 0; i < 4; ++i) {
            blit(OVERLAY_CALLS[i], saved, (int)i * 8, 0);
        }
        WORD ink = (WORD)(0xf800 + variant);
        for (unsigned p = 0; p < RADAR_PIXELS; ++p) {
            frame_art_bits[p] = p % 3 ? ink : 0x07e0;
        }
        if (variant % 3 == 2) {
            *(int *)(frame_art + DIB_WIDTH) = 128;
            *(int *)(frame_art + DIB_HEIGHT) = 128;
            *(int *)(frame_art + DIB_PITCH) = 256;
            *(WORD **)(frame_art + DIB_BITS) = frame_art_bits;
            frame_piece(OVERLAY_CALLS[4], 128, 36, 0, 0, 0, 0);
            frame_piece(OVERLAY_CALLS[4], 128, 36, 0, 92, 0, 92);
            frame_piece(OVERLAY_CALLS[5], 32, 56, 0, 36, 0, 36);
            frame_piece(OVERLAY_CALLS[5], 32, 56, 96, 36, 96, 36);
            CHECK(g_frame_calls == 4);
        } else {
            for (unsigned i = 0; i < 8; ++i) {
                int width = pieces[i].right - pieces[i].left;
                int height = pieces[i].bottom - pieces[i].top;
                *(int *)(frame_art + DIB_WIDTH) = width;
                *(int *)(frame_art + DIB_HEIGHT) = height;
                *(int *)(frame_art + DIB_PITCH) = width * 2;
                *(WORD **)(frame_art + DIB_BITS) = frame_art_bits;
                frame_piece(OVERLAY_CALLS[6 + i], width, height, 0, 0, pieces[i].left, pieces[i].top);
            }
            CHECK(g_frame_calls == 8);
        }
        finish_pixels();
        radar_leave(NULL);
        smooth_radar_finish(1);
        CHECK(record.radar.valid && record.radar.final[1] == ink);
        /* A missing/read-failed part uses the last complete layers. */
        begin();
        DWORD failed[25] = {0};
        failed[10] = MOBILE_PROJECT_CALL + 5;
        failed[11] = 1;
        project_enter(failed);
        CHECK(!g_complete && g_capture.failure == RADAR_READ && !g_frame_calls);
        finish_pixels();
        radar_leave(NULL);
        smooth_radar_finish(1);
        CHECK(record.radar.valid && record.radar.final[1] == ink);
    }
    puts("radar frames: day/M/night switching, all 8 pieces/4 night rects, read reuse OK");
}

static void check_pointers(void)
{
    begin();
    *(int *)(radar + RADAR_WIND_WIDTH) = 13;
    *(unsigned *)(radar + RADAR_WIND_ANGLE) = 0;
    check_wind(58, 10);
    CHECK(g_complete && g_capture.pointers[RADAR_WIND].valid);
    CHECK(g_capture.pointers[RADAR_WIND].angle == 0);
    /* Preserve the frame under the moving pointer, including same-colour ink. */
    *(int *)(frame_art + DIB_WIDTH) = 128;
    *(int *)(frame_art + DIB_HEIGHT) = 128;
    *(int *)(frame_art + DIB_PITCH) = 256;
    for (unsigned p = 0; p < RADAR_PIXELS; ++p) {
        frame_art_bits[p] = 0xffff;
    }
    frame_piece(OVERLAY_CALLS[4], 13, 13, 0, 0, 58, 10);
    check_wind(58, 10);
    CHECK(g_capture.overlay_mask[16 * RADAR_SIDE + 64] & RADAR_FRAME_MASK);
    CHECK(g_capture.overlay[16 * RADAR_SIDE + 64] == frame_art_bits[6 * 128 + 6]);

    *(int *)(frame_art + DIB_WIDTH) = 13;
    *(int *)(frame_art + DIB_HEIGHT) = 13;
    *(int *)(frame_art + DIB_PITCH) = 26;
    *(WORD **)(frame_art + DIB_BITS) = frame_art_bits;
    for (unsigned p = 0; p < 13 * 13; ++p) {
        frame_art_bits[p] = 0xffff;
    }
    pose[3] = 90;
    frame_piece(HEADING_CALL, 13, 13, 0, 0, 106, 58);
    CHECK(g_capture.pointers[RADAR_HEADING].valid);
    CHECK(g_capture.pointers[RADAR_HEADING].angle == 90);
    BYTE source[0x20] = {0};
    *(BYTE **)(ship + SHIP_HEADING_SOURCE) = source;
    *(double *)(source + HEADING_SOURCE_ANGLE) = 180;
    frame_piece(HEADING_CALL, 13, 13, 0, 0, 58, 106);
    CHECK(g_capture.pointers[RADAR_HEADING].valid && g_capture.pointers[RADAR_HEADING].angle == 180);
    source[HEADING_SOURCE_FLAG] = 1;
    frame_piece(HEADING_CALL, 13, 13, 0, 0, 106, 58);
    CHECK(g_capture.pointers[RADAR_HEADING].valid && g_capture.pointers[RADAR_HEADING].angle == 90);
    *(BYTE **)(ship + SHIP_HEADING_SOURCE) = (BYTE *)1;
    frame_piece(HEADING_CALL, 13, 13, 0, 0, 106, 58);
    CHECK(g_complete && !g_capture.pointers[RADAR_HEADING].valid);
    CHECK(g_capture.overlay_mask[58 * RADAR_SIDE + 106] & RADAR_HEADING_MASK);
    *(BYTE **)(ship + SHIP_HEADING_SOURCE) = NULL;
    pose[3] = 0;
    fixture_night = 1;
    check_wind(58, 23);
    CHECK(g_capture.pointers[RADAR_WIND].valid && g_capture.pointers[RADAR_WIND].radius == 35);
    fixture_night = 0;

    g_compass_calls = 4;
    g_frame_calls = 1;
    finish_pixels();
    radar_leave(NULL);
    smooth_radar_finish(1);
    CHECK(record.radar.valid && record.radar.pointers[RADAR_WIND].angle == 0);
    /* A failed earlier layer does not suppress fresh pointer observations. */
    begin();
    g_complete = 0;
    g_capture.failure = RADAR_READ;
    *(unsigned *)(radar + RADAR_WIND_ANGLE) = 180;
    check_wind(106, 58);
    CHECK(!g_complete && g_capture.failure == RADAR_READ);
    CHECK(g_capture.pointers[RADAR_WIND].valid && g_capture.pointers[RADAR_WIND].angle == 90);
    finish_pixels();
    radar_leave(NULL);
    smooth_radar_finish(1);
    CHECK(record.radar.valid && record.radar.pointers[RADAR_WIND].angle == 90);
    CHECK(g_last.pointers[RADAR_WIND].angle == 90);
    /* An unreadable sprite must never publish a partially copied pointer. */
    begin();
    DWORD failed[21] = {0};
    failed[6] = 1;
    failed[10] = HEADING_CALL + 5;
    blit_enter(failed);
    CHECK(!g_complete && g_pointer_failed && !g_capture.pointers[RADAR_HEADING].present);
    radar_leave(NULL);
    smooth_radar_finish(1);
    CHECK(!record.radar.valid && record.radar.failure == RADAR_READ && !g_last.valid);
    puts("radar capture: heading source selection/SEH, displayed wind/frame, rect fallback, night, fresh "
         "partial OK");
}

int main(void)
{
    setup();
    check_radar_bridge();
    fixture_frame = frame;
    begin();
    DWORD saved[25] = {0};
    POINT world = {2367, 2367};
    POINT projected = {64, 64};
    RECT clip = {0, 0, 128, 128};
    saved[1] = (DWORD)ship;
    saved[10] = PORT_PROJECT_CALL + 5;
    saved[11] = (DWORD)&world;
    saved[24] = 3;
    project_enter(saved);
    CHECK(g_has_pending && g_pending.id == 3 && g_pending.x == 2367);
    saved[10] = PORT_CALL + 5;
    saved[11] = (DWORD)frame;
    saved[12] = (DWORD)&clip;
    saved[13] = (DWORD)&projected;
    saved[15] = 0xabcd0000; // game initializes only the low size byte
    marker_enter(saved);
    CHECK(g_has_admitted);
    blit(PORT_BLIT, saved, 60, 60);
    CHECK(g_complete && g_capture.count == 1 && g_capture.markers[0].key == 0x07e0);
    CHECK(g_capture.markers[0].source_rect.right == 8);
    for (unsigned i = 0; i < 4; ++i) {
        blit(OVERLAY_CALLS[i], saved, (int)i * 8, 0);
    }
    blit(OVERLAY_CALLS[4], saved, 0, 120);
    CHECK(g_complete && g_capture.overlay_mask[120 * RADAR_SIDE + 1] &&
          !g_capture.overlay_mask[120 * RADAR_SIDE]);
    check_wind(90, 50);
    finish_pixels();
    radar_leave(NULL);
    CHECK(g_capture.valid);
    check_reuse();
    check_optional_pose();
    check_frame_variants();
    check_pointers();
    begin();
    blit(OVERLAY_CALLS[0], saved, 0, 0);
    g_compass_calls = 4;
    g_frame_calls = 1;
    finish_pixels();
    frame_bits[64 * SCENE_WIDTH + 64] ^= 1;
    radar_leave(NULL);
    CHECK(!g_capture.valid);
    CHECK(g_capture.failure == RADAR_REPLAY);
    begin();
    g_has_admitted = 1;
    g_capture.count = RADAR_MARKERS;
    blit(PORT_BLIT, saved, 60, 60);
    CHECK(!g_complete && g_capture.count == RADAR_MARKERS);
    radar_leave(NULL);
    CHECK(!g_capture.valid);
    CHECK(g_capture.failure == RADAR_OVERFLOW);
    begin();
    saved[6] = 1;
    blit_enter(saved);
    CHECK(!g_complete);
    radar_leave(NULL);
    puts("radar capture: native bridge, port identity, key mask, replay, overflow and SEH fallback OK");
    return 0;
}
