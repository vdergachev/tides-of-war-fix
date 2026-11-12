#include "smooth_balls.h"
#include "smooth_fire.h"
#include "patch.h"
#include "log.h"
#include <string.h>

/* Supported image: constructor, Objects_Draw caller, and sprite raster caller.
 * Only displaced original calls execute; observers never call game code. */
#define BALL_CTOR            0x439080
#define BALL_CTOR_RESUME     0x439087
#define BALL_CTOR_ALT        0x439100
#define BALL_CTOR_ALT_RESUME 0x439108
#define BALL_VTABLE          0x4a40bc
#define OBJECT_CALL          0x43add1
#define OBJECT_DRAW          0x46d0c0
#define SPRITE_CALL          0x43ec96
#define SURFACE_BLIT         0x4179e0
#define DIB_BITS             0x60
#define DIB_PITCH            0x64
#define BALL_TYPE            8
#define BALL_X               0x10
#define BALL_Y               0x18
#define BALL_START_X         0x20
#define BALL_START_Y         0x28
#define BALL_VX              0x30
#define BALL_VY              0x38
#define BALL_LIFE            0x40
#define BALL_TERMINATED      0x4a
#define BALL_HALF_LIFE       0x52
#define ID_LIMIT             (BALL_LIMIT * 2)
#define OBJECT_LIMIT         4096

typedef struct {
    uint64_t generation;
    uintptr_t ptr;
    unsigned seen;
} ball_identity;

static const BYTE CTOR_BYTES[] = {0x8b, 0x44, 0x24, 0x04, 0x56, 0x8b, 0xf1};
static const BYTE CTOR_ALT_BYTES[] = {0x56, 0x8b, 0xf1, 0x68, 0xff, 0xff, 0x00, 0x00};
static const BYTE OBJECT_BYTES[] = {0xe8, 0xea, 0x22, 0x03, 0x00};
static const BYTE SPRITE_BYTES[] = {0xe8, 0x45, 0x8d, 0xfd, 0xff};
static DWORD g_ctor_resume = BALL_CTOR_RESUME;
static DWORD g_ctor_alt_resume = BALL_CTOR_ALT_RESUME;
static DWORD g_object_draw = OBJECT_DRAW;
static DWORD g_surface_blit = SURFACE_BLIT;
static int g_enabled;
static int g_ready;
static int g_mid;
static int g_complete;
static BYTE *g_frame;
static scene_record *g_record;
static WORD g_expected[SCENE_PIXELS];
static ball_identity g_identity[ID_LIMIT];
static scene_ball g_previous[BALL_LIMIT];
static unsigned g_previous_count;
static int g_previous_valid;
static uint64_t g_generation;
static unsigned g_step;
static unsigned g_last_step;
static LONG g_balls;
static LONG g_fallback;

void smooth_balls_configure(const char *path)
{
    g_enabled = (int)GetPrivateProfileIntA("smooth_balls", "enabled", 0, path) != 0;
    log_line("[smooth_balls] enabled=%d", g_enabled);
}

static ball_identity *identity(uintptr_t ptr, int birth)
{
    ball_identity *free_slot = NULL;
    unsigned hash = ball_hash(ptr, 0);

    for (unsigned i = 0; i < ID_LIMIT; ++i) {
        ball_identity *slot = &g_identity[(hash + i) & (ID_LIMIT - 1)];
        if (slot->ptr == ptr) {
            return slot;
        }

        if (!slot->ptr) {
            if (!free_slot) {
                free_slot = slot;
            }
            break;
        }

        if (!free_slot && g_step > slot->seen && g_step - slot->seen > 2) {
            free_slot = slot;
        }
    }

    if (birth && free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->ptr = ptr;
        return free_slot;
    }

    return NULL;
}

static void __stdcall observe_birth(const DWORD *saved)
{
    if (!g_ready) {
        return;
    }

    DWORD error = GetLastError();
    ball_identity *slot = identity(saved[6], 1);
    if (slot) {
        slot->generation = ++g_generation;
        slot->seen = g_step;
    }

    SetLastError(error);
}

void smooth_balls_begin(scene_record *record, BYTE *frame)
{
    smooth_fire_begin(record, frame);
    g_mid = 0;
    g_record = record;
    g_frame = frame;
    g_step = record->step;
    record->balls_valid = 0;
    record->ball_count = 0;
    record->ball_births = 0;
    record->ball_ends = 0;
    record->ball_event_count = 0;
    g_complete = g_ready && g_enabled && record->valid;
    if (g_complete) {
        memcpy(record->pixels[SCENE_NO_BALLS], record->pixels[SCENE_A1], sizeof(g_expected));
        memset(record->ball_mid_mask, 0, sizeof(record->ball_mid_mask));
    }
}

static int capture_ball(const BYTE *object)
{
    if (*(const DWORD *)object != BALL_VTABLE) {
        return 0;
    }

    if (object[BALL_TERMINATED]) {
        return 1;
    }

    ball_identity *slot = identity((uintptr_t)object, 0);
    if (!slot || !slot->generation || g_record->ball_count == BALL_LIMIT) {
        return 0;
    }

    scene_ball *ball = &g_record->balls[g_record->ball_count++];
    ball->ptr = (uintptr_t)object;
    ball->generation = slot->generation;
    ball->type = *(const unsigned *)(object + BALL_TYPE);
    ball->x = *(const double *)(object + BALL_X);
    ball->y = *(const double *)(object + BALL_Y);
    ball->vx = *(const double *)(object + BALL_VX);
    ball->vy = *(const double *)(object + BALL_VY);
    ball->start_x = *(const double *)(object + BALL_START_X);
    ball->start_y = *(const double *)(object + BALL_START_Y);
    ball->life = *(const WORD *)(object + BALL_LIFE);
    ball->half_life = *(const WORD *)(object + BALL_HALF_LIFE);
    if (!(fabs(ball->x) < 1e8 && fabs(ball->y) < 1e8 && fabs(ball->vx) < 1e5 && fabs(ball->vy) < 1e5 &&
          fabs(ball->start_x) < 1e8 && fabs(ball->start_y) < 1e8)) {
        return 0;
    }

    slot->seen = g_step;
    return 1;
}

static void __stdcall observe_objects(const DWORD *saved)
{
    g_mid = 0;
    if (!g_complete || !g_record || g_record->step != g_step) {
        return;
    }

    DWORD error = GetLastError();
    __try {
        const DWORD *args = saved + 11;
        if ((BYTE *)args[0] != g_frame) {
            g_complete = 0;
            __leave;
        }

        const POINT *camera = (const POINT *)args[1];
        const POINT *view = (const POINT *)args[2];
        g_record->ball_offset.x = camera->x - view->x;
        g_record->ball_offset.y = camera->y - view->y;
        const DWORD *node = *(const DWORD **)(saved[6] + 4);
        unsigned visited = 0;
        while (node && visited++ < OBJECT_LIMIT) {
            const BYTE *object = (const BYTE *)node[2];
            if (object && !capture_ball(object)) {
                g_complete = 0;
                break;
            }
            node = (const DWORD *)node[0];
        }

        if (node) {
            g_complete = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_complete = 0;
    }

    SetLastError(error);
}

static void __stdcall observe_objects_leave(const DWORD *saved)
{
    (void)saved;
    smooth_fire_objects_done();
    if (!g_complete) {
        return;
    }

    DWORD error = GetLastError();
    __try {
        const BYTE *bits = *(const BYTE **)(g_frame + DIB_BITS);
        int pitch = *(const int *)(g_frame + DIB_PITCH);
        if (!bits || pitch < -65536 || pitch > 65536 || abs(pitch) < SCENE_WIDTH * 2) {
            g_complete = 0;
            __leave;
        }

        for (int y = 0; y < SCENE_HEIGHT; ++y) {
            memcpy(g_expected + y * SCENE_WIDTH, bits + y * pitch, SCENE_WIDTH * 2);
        }

        g_mid = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_complete = 0;
    }

    SetLastError(error);
}

/* Mirror source-key writes, never differences of baked backgrounds. This keeps
 * coverage even when an opaque smoke pixel equals the underlying hull/sea. */
static void mirror_sprite(const DWORD *saved)
{
    const DWORD *args = saved + 11;
    if ((BYTE *)args[0] != g_frame) {
        return;
    }

    const BYTE *source = (const BYTE *)saved[6];
    int width = *(const int *)args[4];
    int height = *(const int *)args[5];
    int sx = *(const int *)args[6];
    int sy = *(const int *)args[7];
    int dx = *(const int *)args[8];
    int dy = *(const int *)args[9];
    int pitch = *(const int *)(source + DIB_PITCH);
    int source_width = *(const int *)(source + 8);
    int source_height = *(const int *)(source + 0x0c);
    if (!*(const DWORD *)(source + 0x1c) || !*(const DWORD *)(g_frame + 0x1c) ||
        *(const WORD *)(source + 0x42) != 16 || *(const DWORD *)(source + 0x44) != 3 ||
        *(const DWORD *)(g_frame + 0x44) != 3 || width <= 0 || height <= 0 || sx < 0 || sy < 0 || dx < 0 ||
        dy < 0 || dx > SCENE_WIDTH - width || dy > SCENE_HEIGHT - height || source_width < 1 ||
        source_width > 32768 || source_height < -32768 || source_height > 32768 ||
        sx > source_width - width || sy > abs(source_height) - height || pitch < -65536 || pitch > 65536 ||
        abs(pitch) < 2 * (sx + width) || (source_height < 0) != (*(const int *)(g_frame + 0x3c) < 0)) {
        g_complete = 0;
        return;
    }

    unsigned r = *(const BYTE *)args[1];
    unsigned g = *(const BYTE *)args[2];
    unsigned b = *(const BYTE *)args[3];
    WORD key = (WORD)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    const BYTE *bits = *(const BYTE **)(source + DIB_BITS);
    for (int y = 0; y < height; ++y) {
        const WORD *row = (const WORD *)(bits + (sy + y) * pitch) + sx;
        for (int x = 0; x < width; ++x) {
            if (row[x] != key) {
                unsigned p = (dy + y) * SCENE_WIDTH + dx + x;
                g_record->pixels[SCENE_NO_BALLS][p] = row[x];
                g_record->ball_mid_mask[p] = 1;
                g_expected[p] = row[x];
            }
        }
    }
}

static void __stdcall observe_sprite(const DWORD *saved)
{
    smooth_fire_sprite(saved);
    if (!g_mid || !g_complete) {
        return;
    }

    DWORD error = GetLastError();
    __try {
        mirror_sprite(saved);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_complete = 0;
    }

    SetLastError(error);
}

static void ball_event(const scene_ball *ball, unsigned kind)
{
    /* At most BALL_LIMIT removals plus BALL_LIMIT births per complete pair. */
    scene_ball_event *event = &g_record->ball_events[g_record->ball_event_count++];
    event->ptr = ball->ptr;
    event->generation = ball->generation;
    event->kind = kind;
    g_record->ball_births += kind == BALL_BIRTH;
    g_record->ball_ends += kind == BALL_END;
}

static void capture_events(void)
{
    if (!g_record->balls_valid) {
        g_previous_valid = 0;
        return;
    }

    if (g_previous_valid && g_step == g_last_step + 1) {
        ball_lookup current;
        ball_lookup previous;
        ball_index(&current, g_record->balls, g_record->ball_count);
        ball_index(&previous, g_previous, g_previous_count);

        for (unsigned i = 0; i < g_previous_count; ++i) {
            if (!ball_lookup_find(&current, g_record->balls, &g_previous[i])) {
                ball_event(&g_previous[i], BALL_END);
            }
        }

        for (unsigned i = 0; i < g_record->ball_count; ++i) {
            if (!ball_lookup_find(&previous, g_previous, &g_record->balls[i])) {
                ball_event(&g_record->balls[i], BALL_BIRTH);
            }
        }
    }

    g_previous_valid = 1;
    g_previous_count = g_record->ball_count;
    memcpy(g_previous, g_record->balls, g_previous_count * sizeof(*g_previous));
}

void smooth_balls_finish(void)
{
    if (!g_record) {
        return;
    }

    g_record->balls_valid =
        g_mid && g_complete && !memcmp(g_expected, g_record->pixels[SCENE_A], sizeof(g_expected));
    smooth_fire_finish();
    capture_events();
    g_mid = 0;
    InterlockedExchangeAdd(&g_balls, g_record->balls_valid ? (LONG)g_record->ball_count : 0);
    InterlockedExchangeAdd(&g_fallback, g_ready && !g_record->balls_valid);
    g_last_step = g_step;
    g_record = NULL;
    g_complete = 0;
}

/* PUSHAD: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX,flags,bridge return,
 * thunk return,args. Each bridge preserves LastError, flags, GPRs and x87/SSE.
 * Wrappers consume only copied arguments, then RET the caller's originals. */
// clang-format off
#define BALL_BRIDGE(name, observer) \
    __declspec(naked) static void name(void) { \
        __asm { pushfd } \
        __asm { pushad } \
        __asm { mov eax, esp } \
        __asm { sub esp, 528 } \
        __asm { and esp, -16 } \
        __asm { fxsave [esp] } \
        __asm { fninit } \
        __asm { push 1f80h } \
        __asm { ldmxcsr [esp] } \
        __asm { add esp, 4 } \
        __asm { push eax } \
        __asm { push eax } \
        __asm { cld } \
        __asm { call observer } \
        __asm { pop eax } \
        __asm { fxrstor [esp] } \
        __asm { mov esp, eax } \
        __asm { popad } \
        __asm { popfd } \
        __asm { ret } \
    }
BALL_BRIDGE(birth_bridge, observe_birth)
BALL_BRIDGE(objects_bridge, observe_objects)
BALL_BRIDGE(objects_leave_bridge, observe_objects_leave)
BALL_BRIDGE(sprite_bridge, observe_sprite)

__declspec(naked) static void ctor_thunk(void)
{
    __asm {
        call birth_bridge
        mov eax, [esp + 4]
        push esi
        mov esi, ecx
        jmp dword ptr [g_ctor_resume]
    }
}

/* Alternate constructor has the same ECX identity at entry. Replay its full
 * PUSH ESI / MOV ESI,ECX / PUSH 0xffff prologue, preserving the original stack. */
__declspec(naked) static void ctor_alt_thunk(void)
{
    __asm {
        call birth_bridge
        push esi
        mov esi, ecx
        push 0ffffh
        jmp dword ptr [g_ctor_alt_resume]
    }
}

__declspec(naked) static void objects_thunk(void)
{
    __asm {
        call objects_bridge
        push [esp + 12]
        push [esp + 12]
        push [esp + 12]
        call dword ptr [g_object_draw]
        call objects_leave_bridge
        ret 12
    }
}

__declspec(naked) static void sprite_thunk(void)
{
    __asm {
        call sprite_bridge
        jmp dword ptr [g_surface_blit]
    }
}

// clang-format on

void smooth_balls_install(void)
{
    if ((!g_enabled && !smooth_fire_enabled()) || g_ready) {
        return;
    }

    if (!patch_check((void *)BALL_CTOR, CTOR_BYTES, sizeof(CTOR_BYTES)) ||
        !patch_check((void *)BALL_CTOR_ALT, CTOR_ALT_BYTES, sizeof(CTOR_ALT_BYTES)) ||
        !patch_check((void *)OBJECT_CALL, OBJECT_BYTES, sizeof(OBJECT_BYTES)) ||
        !patch_check((void *)SPRITE_CALL, SPRITE_BYTES, sizeof(SPRITE_BYTES))) {
        log_line("smooth_balls: hook bytes rejected; original balls");
        return;
    }

    g_ready =
        patch_jmp_padded((void *)BALL_CTOR, CTOR_BYTES, sizeof(CTOR_BYTES), (void *)(uintptr_t)ctor_thunk) &&
        patch_jmp_padded((void *)BALL_CTOR_ALT, CTOR_ALT_BYTES, sizeof(CTOR_ALT_BYTES),
                         (void *)(uintptr_t)ctor_alt_thunk) &&
        patch_call((void *)OBJECT_CALL, OBJECT_BYTES, (void *)(uintptr_t)objects_thunk) &&
        patch_call((void *)SPRITE_CALL, SPRITE_BYTES, (void *)(uintptr_t)sprite_thunk);
    log_line("smooth_balls: ready=%d limit=%u original_frame_preserved=1", g_ready, BALL_LIMIT);
    if (g_ready) {
        smooth_fire_install();
    }
}

void smooth_balls_log(void)
{
    smooth_fire_log();
    if (g_ready) {
        diag_log("smooth_balls: balls=%ld fallback_records=%ld", InterlockedExchange(&g_balls, 0),
                 InterlockedExchange(&g_fallback, 0));
    }
}
