#include "smooth_fire.h"
#include "patch.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

/* Read-only observers of the supported image's damage and drifting effects. */
#define DAMAGE_SMOKE_CALL 0x451537
#define DAMAGE_SMOKE_FN   0x43d220
#define FIRE_CALL         0x451578
#define EFFECT_FN         0x43d9d0
#define DRIFT_CALL        0x43d747
#define BIRTH_CALL        0x43d175
#define ADD_TAIL_FN       0x494568
#define DIB_BITS          0x60
#define DIB_PITCH         0x64
#define EFFECT_TYPE       8
#define VIEW_TICK         0x4c
#define SMOKE_IDENTITIES  2048

typedef struct {
    uintptr_t object;
    uintptr_t owner;
    unsigned id;
    unsigned seen;
} smoke_identity;

static const BYTE SMOKE_BYTES[] = {0xe8, 0xe4, 0xbc, 0xfe, 0xff};
static const BYTE FIRE_BYTES[] = {0xe8, 0x53, 0xc4, 0xfe, 0xff};
static const BYTE DRIFT_BYTES[] = {0xe8, 0x84, 0x02, 0x00, 0x00};
static const BYTE BIRTH_BYTES[] = {0xe8, 0xee, 0x73, 0x05, 0x00};
static DWORD g_smoke_fn = DAMAGE_SMOKE_FN;
static DWORD g_effect_fn = EFFECT_FN;
static DWORD g_add_tail_fn = ADD_TAIL_FN;
static int g_enabled;
static int g_ready;
static int g_complete;
static int g_mid;
static BYTE *g_frame;
static scene_record *g_record;
static uintptr_t g_spawn_owner;
static unsigned g_spawn_id;
static uintptr_t g_owner;
static unsigned g_owner_id;
static unsigned g_sprite_frame;
static smoke_identity g_smoke[SMOKE_IDENTITIES];
static WORD g_expected[SCENE_PIXELS];
static LONG g_captured;
static LONG g_fallback;

void smooth_fire_configure(const char *path)
{
    g_enabled = (int)GetPrivateProfileIntA("smooth_fire", "enabled", 0, path) != 0;
    log_line("[smooth_fire] enabled=%d", g_enabled);
}

int smooth_fire_enabled(void)
{
    return g_enabled;
}

static smoke_identity *smoke_slot(uintptr_t object, int birth)
{
    smoke_identity *free_slot = NULL;
    unsigned hash = (unsigned)(object >> 4);
    for (unsigned i = 0; i < SMOKE_IDENTITIES; ++i) {
        smoke_identity *slot = &g_smoke[(hash + i) & (SMOKE_IDENTITIES - 1)];
        if (slot->object == object) {
            return slot;
        }
        if (!slot->object) {
            if (!free_slot) {
                free_slot = slot;
            }
            break;
        }
        if (!free_slot && g_record && g_record->step > slot->seen && g_record->step - slot->seen > 64) {
            free_slot = slot;
        }
    }
    return birth ? free_slot : NULL;
}

void smooth_fire_begin(scene_record *record, BYTE *frame)
{
    g_record = record;
    g_frame = frame;
    record->fire_valid = 0;
    record->effect_count = 0;
    g_owner = 0;
    g_spawn_owner = 0;
    g_mid = 0;
    g_complete = g_ready && record->valid;
}

void smooth_fire_objects_done(void)
{
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
        memcpy(g_record->pixels[SCENE_NO_FIRE], g_expected, sizeof(g_expected));
        g_mid = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_complete = 0;
    }
    SetLastError(error);
}

static void __stdcall observe_spawn(const DWORD *saved)
{
    /* EDI remains the owning ship at the damage-smoke factory call. */
    g_spawn_owner = g_ready ? saved[0] : 0;
    g_spawn_id = 0xffff;
    if (g_record) {
        for (unsigned i = 0; i < g_record->count; ++i) {
            if (g_record->ships[i].ptr == g_spawn_owner) {
                g_spawn_id = g_record->ships[i].id;
                break;
            }
        }
    }
}

static void __stdcall observe_spawn_leave(const DWORD *saved)
{
    (void)saved;
    g_spawn_owner = 0;
}

static void __stdcall observe_birth(const DWORD *saved)
{
    if (!g_ready) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        uintptr_t object = saved[11];
        smoke_identity *slot = smoke_slot(object, 1);
        if (slot) {
            slot->object = object;
            slot->owner = g_spawn_id == 0xffff ? 0 : g_spawn_owner;
            slot->id = g_spawn_id;
            slot->seen = g_record ? g_record->step : 0;
        } else if (g_spawn_owner) {
            g_complete = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_complete = 0;
    }
    SetLastError(error);
}

static void __stdcall observe_fire(const DWORD *saved)
{
    g_owner = 0;
    if (!g_complete || !g_mid) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        g_owner = saved[0];
        g_owner_id = 0xffff;
        g_sprite_frame = *(const WORD *)(saved[6] + VIEW_TICK) % 3;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_complete = 0;
    }
    SetLastError(error);
}

static void __stdcall observe_drift(const DWORD *saved)
{
    g_owner = 0;
    if (!g_complete || !g_mid) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        const BYTE *object = (const BYTE *)saved[1]; /* ESI: CEffectObject. */
        unsigned type = *(const unsigned *)(object + EFFECT_TYPE);
        unsigned life = saved[15]; /* Original lifetime argument to EffectSprite. */
        g_sprite_frame = type == 0 ? 28 - life : (type == 1 ? 10 - life : 13 - life);
        if (type == 6) {
            smoke_identity *slot = smoke_slot((uintptr_t)object, 0);
            if (!slot || !slot->owner) {
                g_complete = 0;
            } else {
                g_owner = slot->owner;
                g_owner_id = slot->id;
                slot->seen = g_record->step;
                g_sprite_frame = 29 - life;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_complete = 0;
    }
    SetLastError(error);
}

static void __stdcall observe_effect_leave(const DWORD *saved)
{
    (void)saved;
    g_owner = 0;
}

static int capture_sprite(const DWORD *saved)
{
    const DWORD *args = saved + 11;
    if ((BYTE *)args[0] != g_frame) {
        return 1;
    }
    const BYTE *source = (const BYTE *)saved[6];
    int width = *(const int *)args[4];
    int height = *(const int *)args[5];
    int sx = *(const int *)args[6];
    int sy = *(const int *)args[7];
    int dx = *(const int *)args[8];
    int dy = *(const int *)args[9];
    int pitch = *(const int *)(source + DIB_PITCH);
    int sw = *(const int *)(source + 8);
    int sh = *(const int *)(source + 0x0c);
    if (!*(const DWORD *)(source + 0x1c) || !*(const DWORD *)(g_frame + 0x1c) ||
        *(const WORD *)(source + 0x42) != 16 || *(const DWORD *)(source + 0x44) != 3 ||
        *(const DWORD *)(g_frame + 0x44) != 3 || width < 1 || height < 1 || width > SCENE_SPRITE_SIZE ||
        height > SCENE_SPRITE_SIZE || sx < 0 || sy < 0 || dx < 0 || dy < 0 || dx > SCENE_WIDTH - width ||
        dy > SCENE_HEIGHT - height || sw < 1 || sw > 32768 || sh < -32768 || sh > 32768 || sx > sw - width ||
        sy > abs(sh) - height || pitch < -65536 || pitch > 65536 || abs(pitch) < 2 * (sx + width) ||
        (sh < 0) != (*(const int *)(g_frame + 0x3c) < 0) || g_record->effect_count == SCENE_EFFECTS) {
        return 0;
    }

    unsigned ship = SCENE_SHIPS;
    if (g_owner) {
        for (ship = 0; ship < g_record->count; ++ship) {
            if (g_record->ships[ship].ptr == g_owner &&
                (g_owner_id == 0xffff || g_record->ships[ship].id == g_owner_id)) {
                break;
            }
        }
        if (ship == g_record->count) {
            return 0;
        }
    }
    scene_effect *effect = &g_record->effects[g_record->effect_count];
    effect->ship = ship;
    effect->id = ship == SCENE_SHIPS ? 0xffff : g_record->ships[ship].id;
    effect->source = (uintptr_t)source;
    effect->frame = g_sprite_frame;
    SetRect(&effect->source_rect, sx, sy, sx + width, sy + height);
    scene_draw *draw = &effect->draw;
    draw->ship = ship;
    draw->id = effect->id;
    draw->width = width;
    draw->height = height;
    draw->stride = pitch;
    unsigned r = *(const BYTE *)args[1];
    unsigned g = *(const BYTE *)args[2];
    unsigned b = *(const BYTE *)args[3];
    draw->key = (WORD)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
    SetRect(&draw->box, dx, dy, dx + width, dy + height);
    effect->relative = draw->box;
    if (ship != SCENE_SHIPS) {
        const POINT *point = &g_record->ships[ship].point;
        OffsetRect(&effect->relative, g_record->camera.x - g_record->view.left - point->x,
                   g_record->camera.y - g_record->view.top - point->y);
    }
    const BYTE *bits = *(const BYTE **)(source + DIB_BITS);
    for (int y = 0; y < height; ++y) {
        const WORD *row = (const WORD *)(bits + (sy + y) * pitch) + sx;
        memcpy(draw->pixels + y * width, row, width * 2);
        for (int x = 0; x < width; ++x) {
            if (row[x] != draw->key) {
                g_expected[(dy + y) * SCENE_WIDTH + dx + x] = row[x];
            }
        }
    }
    ++g_record->effect_count;
    return 1;
}

void smooth_fire_sprite(const DWORD *saved)
{
    if (!g_mid || !g_complete) {
        return;
    }
    DWORD error = GetLastError();
    __try {
        g_complete = capture_sprite(saved);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_complete = 0;
    }
    SetLastError(error);
}

void smooth_fire_finish(void)
{
    if (!g_record) {
        return;
    }
    g_record->fire_valid = g_mid && g_complete && g_record->valid &&
                           !memcmp(g_expected, g_record->pixels[SCENE_A], sizeof(g_expected));
    if (g_record->fire_valid && g_record->balls_valid) {
        /* All MID sprites are redrawn in order; no ball or sprite remains baked. */
        memcpy(g_record->pixels[SCENE_NO_FIRE], g_record->pixels[SCENE_A1], sizeof(g_expected));
    }
    InterlockedExchangeAdd(&g_captured, g_record->fire_valid ? (LONG)g_record->effect_count : 0);
    InterlockedExchangeAdd(&g_fallback, g_ready && !g_record->fire_valid);
    g_record = NULL;
    g_complete = 0;
    g_mid = 0;
    g_owner = 0;
}

/* PUSHAD: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX,flags,bridge return,
 * thunk return,args. Preserve LastError in observers and flags/GPRs/FP here.
 * Wrappers copy arguments for the displaced original call, then RET originals. */
// clang-format off
#define FIRE_BRIDGE(name, observer) \
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
FIRE_BRIDGE(spawn_bridge, observe_spawn)
FIRE_BRIDGE(spawn_leave_bridge, observe_spawn_leave)
FIRE_BRIDGE(birth_bridge, observe_birth)
FIRE_BRIDGE(fire_bridge, observe_fire)
FIRE_BRIDGE(drift_bridge, observe_drift)
FIRE_BRIDGE(effect_leave_bridge, observe_effect_leave)

__declspec(naked) static void spawn_thunk(void)
{
    __asm {
        call spawn_bridge
        push [esp + 12]
        push [esp + 12]
        push [esp + 12]
        call dword ptr [g_smoke_fn]
        call spawn_leave_bridge
        ret 12
    }
}

__declspec(naked) static void birth_thunk(void)
{
    __asm {
        call birth_bridge
        jmp dword ptr [g_add_tail_fn]
    }
}

#define EFFECT_THUNK(name, bridge) \
    __declspec(naked) static void name(void) { \
        __asm { call bridge } \
        __asm { push [esp + 20] } \
        __asm { push [esp + 20] } \
        __asm { push [esp + 20] } \
        __asm { push [esp + 20] } \
        __asm { push [esp + 20] } \
        __asm { call dword ptr [g_effect_fn] } \
        __asm { call effect_leave_bridge } \
        __asm { ret 20 } \
    }
EFFECT_THUNK(fire_thunk, fire_bridge)
EFFECT_THUNK(drift_thunk, drift_bridge)
// clang-format on

void smooth_fire_install(void)
{
    if (!g_enabled || g_ready) {
        return;
    }
    if (!patch_check((void *)DAMAGE_SMOKE_CALL, SMOKE_BYTES, sizeof(SMOKE_BYTES)) ||
        !patch_check((void *)FIRE_CALL, FIRE_BYTES, sizeof(FIRE_BYTES)) ||
        !patch_check((void *)DRIFT_CALL, DRIFT_BYTES, sizeof(DRIFT_BYTES)) ||
        !patch_check((void *)BIRTH_CALL, BIRTH_BYTES, sizeof(BIRTH_BYTES))) {
        log_line("smooth_fire: hook bytes rejected; original fire");
        return;
    }
    g_ready = patch_call((void *)DAMAGE_SMOKE_CALL, SMOKE_BYTES, (void *)(uintptr_t)spawn_thunk) &&
              patch_call((void *)FIRE_CALL, FIRE_BYTES, (void *)(uintptr_t)fire_thunk) &&
              patch_call((void *)DRIFT_CALL, DRIFT_BYTES, (void *)(uintptr_t)drift_thunk) &&
              patch_call((void *)BIRTH_CALL, BIRTH_BYTES, (void *)(uintptr_t)birth_thunk);
    log_line("smooth_fire: ready=%d limit=%u original_frame_preserved=1", g_ready, SCENE_EFFECTS);
}

void smooth_fire_log(void)
{
    if (g_ready) {
        diag_log("smooth_fire: sprites=%ld fallback_records=%ld", InterlockedExchange(&g_captured, 0),
                 InterlockedExchange(&g_fallback, 0));
    }
}
