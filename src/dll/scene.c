#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include "scene.h"
#include "patch.h"
#include "log.h"

/* W60-W63, supported image only. All offsets are observation-only. */
#define CLOUD_CALL_ADDRESS     0x43ae82
#define CLOUD_ADDRESS          0x43dbe0
#define CLOUD_FLAGS_ADDRESS    0x4cde38
#define CLOUD_STATE            0x1430
#define CLOUD_STRIDE           0x38
#define CLOUD_KEY_BITMAP       0x584
#define CLOUD_BLEND_BITMAP     0xcd4
#define CLOUD_BITMAP_STRIDE    0x68
#define DRAW_STATE_ADDRESS     0x4cde48
#define FRAME_ADDRESS          0x4cde34
#define CAMERA_ADDRESS         0x4cdde8
#define GAME_TABLE_ADDRESS     0x4cddd8
#define PASS_ADDRESS           0x455d90
#define HULL_CALL_ADDRESS      0x43adbb
#define SHIP_PASS_CALL_ADDRESS 0x43ae21
#define SHIP_CALL_ADDRESS      0x455fb5
#define SHIP_ADDRESS           0x456d30
#define BLEND_CALL_ADDRESS     0x417f2b
#define BLEND_ADDRESS          0x418020
#define BLIT_CALL_ADDRESS      0x418960
#define BLIT_ADDRESS           0x417b40
#define STEP_ADDRESS           0x42ae70
#define STEP_RESUME_ADDRESS    (STEP_ADDRESS + 8)
#define DIB_WIDTH              8
#define DIB_HEIGHT             0x0c
#define DIB_HDC                0x24
#define DIB_BPP                0x42
#define DIB_BITS               0x60
#define DIB_PITCH              0x64
#define CAMERA_RECT            8
#define CAMERA_FOLLOW          0x48
#define SLOT_ID                0x154
#define TABLE_ARRAY            0xa40
#define TABLE_COUNT            0xa44
#define SHIP_TYPE              0x48
#define SHIP_POSE              0x28
#define SHIP_POINT             0x4c

static const BYTE CLOUD_BYTES[] = {0xe8, 0x59, 0x2d, 0x00, 0x00};
static DWORD g_cloud_fn = CLOUD_ADDRESS;
static const BYTE HULL_BYTES[] = {0xe8, 0xd0, 0xaf, 0x01, 0x00};
static const BYTE PASS_BYTES[] = {0xe8, 0x6a, 0xaf, 0x01, 0x00};
static const BYTE SHIP_BYTES[] = {0xe8, 0x76, 0x0d, 0x00, 0x00};
static const BYTE BLEND_BYTES[] = {0xe8, 0xf0, 0x00, 0x00, 0x00};
static const BYTE BLIT_BYTES[] = {0xe8, 0xdb, 0xf1, 0xff, 0xff};
static const BYTE STEP_BYTES[] = {0x64, 0xa1, 0, 0, 0, 0, 0x6a, 0xff};
static DWORD g_pass_fn = PASS_ADDRESS;
static DWORD g_ship_fn = SHIP_ADDRESS;
static DWORD g_blend_fn = BLEND_ADDRESS;
static DWORD g_blit_fn = BLIT_ADDRESS;
static DWORD g_step_resume = STEP_RESUME_ADDRESS;
static SRWLOCK *g_lock;
static scene_record g_storage[SCENE_RING + 1];
static scene_record *g_work = &g_storage[SCENE_RING];
static scene_record *g_queue[SCENE_RING];
static unsigned g_head;
static unsigned g_size;
static unsigned g_step;
static unsigned g_last_step;
static int g_had_record;
static int g_open;
static int g_mode;
static int g_current = -1;
static BYTE *g_frame;
static double g_cost;
static double g_cost_max;
static double g_step_cost;
static unsigned g_records;
static unsigned g_failures;
static unsigned g_overruns;
static unsigned g_coalesced;
static DWORD g_last_log;
static double g_published_cost;
static double g_published_max;
static unsigned g_published_failures;
static LONGLONG g_frequency;

static LONGLONG ticks(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static int mission(void)
{
    int state = *(int *)DRAW_STATE_ADDRESS;
    return state == 2 || state == 3;
}

static int copy_layer(int layer)
{
    if (g_frame != *(BYTE **)FRAME_ADDRESS || *(int *)(g_frame + DIB_WIDTH) != SCENE_WIDTH ||
        *(int *)(g_frame + DIB_HEIGHT) != SCENE_HEIGHT || *(WORD *)(g_frame + DIB_BPP) != 16) {
        return 0;
    }
    BYTE *bits = *(BYTE **)(g_frame + DIB_BITS);
    int pitch = *(int *)(g_frame + DIB_PITCH);
    if (!bits || pitch < -65536 || pitch > 65536 || abs(pitch) < SCENE_WIDTH * 2 || !GdiFlush()) {
        return 0;
    }
    for (int y = 0; y < SCENE_HEIGHT; ++y) {
        memcpy(g_work->pixels[layer] + y * SCENE_WIDTH, bits + y * pitch, SCENE_WIDTH * 2);
    }
    g_work->layers |= 1u << layer;
    return 1;
}

/* Saved PUSHAD image: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX,flags,bridge-return,
 * thunk-return,args. FXSAVE/FXRSTOR protects the game's x87 and SSE state. */
static void __stdcall observe_enter(const DWORD *saved)
{
    DWORD error = GetLastError();
    LONGLONG start = ticks();
    __try {
        const DWORD *args = saved + 11;
        int mode = (int)(args[1] & 255);
        g_mode = mode;
        g_current = -1;
        if (!g_lock || !mission()) {
            g_open = 0;
            __leave;
        }
        if (mode == 1) {
            g_step_cost = 0;
            memset(g_work, 0, offsetof(scene_record, pixels));
            g_open = 1;
            g_frame = (BYTE *)args[0];
            g_work->step = g_step;
            g_work->cloud_flags = *(BYTE *)CLOUD_FLAGS_ADDRESS;
            g_work->qpc = start;
            g_work->view = *(RECT *)args[3];
            BYTE *camera = *(BYTE **)CAMERA_ADDRESS;
            RECT world = *(RECT *)(camera + CAMERA_RECT);
            g_work->camera.x = world.left;
            g_work->camera.y = world.top;
            if (world.right <= world.left || world.bottom <= world.top) {
                g_work->camera = *(POINT *)args[2];
            }
            g_work->follow = *(WORD *)(camera + CAMERA_FOLLOW);
            RECT v = g_work->view;
            g_work->valid = v.left >= 0 && v.top >= 0 && v.right <= SCENE_WIDTH && v.bottom <= SCENE_HEIGHT &&
                            v.right > v.left + 16 && v.bottom > v.top + 16;
        }
        if (!g_open || g_frame != (BYTE *)args[0] || (mode != 0 && mode != 1)) {
            g_open = 1;
            g_frame = (BYTE *)args[0];
            g_work->step = g_step;
            g_work->layers = 0;
            g_work->valid = 0;
            __leave;
        }
        if (!copy_layer(mode ? SCENE_A0 : SCENE_A)) {
            g_work->valid = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_work->valid = 0;
    }
    double cost = (double)(ticks() - start);
    g_cost += cost;
    g_step_cost += cost;
    SetLastError(error);
}

static void __stdcall observe_leave(const DWORD *saved)
{
    (void)saved;
    DWORD error = GetLastError();
    LONGLONG start = ticks();
    __try {
        g_current = -1;
        if (g_open && !copy_layer(g_mode ? SCENE_A1 : SCENE_B)) {
            g_work->valid = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_work->valid = 0;
    }
    double cost = (double)(ticks() - start);
    g_cost += cost;
    g_step_cost += cost;
    SetLastError(error);
}

static void __stdcall observe_ship(const DWORD *saved)
{
    DWORD error = GetLastError();
    LONGLONG start = ticks();
    g_current = -1;
    __try {
        if (!g_open || g_mode != 0 || !g_work->valid) {
            __leave;
        }
        BYTE *slot = (BYTE *)saved[6];
        unsigned id = *(WORD *)(slot + SLOT_ID);
        if (id == 0xffff) {
            __leave;
        }
        BYTE *table = *(BYTE **)GAME_TABLE_ADDRESS;
        if (!table || id >= *(unsigned *)(table + TABLE_COUNT)) {
            g_work->valid = 0;
            __leave;
        }
        BYTE *ship = (*(BYTE ***)(table + TABLE_ARRAY))[id];
        unsigned type = *(WORD *)(ship + SHIP_TYPE);
        if (type == 9 || type == 10) {
            __leave;
        }
        if (g_work->count == SCENE_SHIPS) {
            g_work->valid = 0;
            __leave;
        }
        scene_ship *out = &g_work->ships[g_work->count++];
        double *pose = *(double **)(ship + SHIP_POSE);
        out->ptr = (uintptr_t)ship;
        out->id = id;
        out->x = pose[0];
        out->y = pose[1];
        out->course = pose[2];
        out->point = *(POINT *)(ship + SHIP_POINT);
        SetRectEmpty(&out->box);
        g_current = (int)g_work->count - 1;
        if (!(fabs(out->x) < 1e8 && fabs(out->y) < 1e8 && fabs(out->course) < 1e8) ||
            abs(out->point.x) > 100000000 || abs(out->point.y) > 100000000) {
            g_work->valid = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_work->valid = 0;
    }
    double cost = (double)(ticks() - start);
    g_cost += cost;
    g_step_cost += cost;
    SetLastError(error);
}

/* The D3D colour-key transfer supplies the clipped destination rect. */
static void __stdcall observe_blit(const DWORD *saved)
{
    DWORD error = GetLastError();
    LONGLONG start = ticks();
    __try {
        if (!g_open || g_current < 0 || !g_work->valid) {
            __leave;
        }
        const DWORD *args = saved + 11;
        if ((BYTE *)saved[6] != g_frame) {
            g_work->valid = 0;
            __leave;
        }
        BYTE *bits = *(BYTE **)(g_frame + DIB_BITS);
        int pitch = *(int *)(g_frame + DIB_PITCH);
        int w = (int)args[2];
        int h = (int)args[3];
        if (!pitch || (int)args[4] != pitch || w < 1 || h < 1 || w > 144 || h > 144) {
            g_work->valid = 0;
            __leave;
        }
        int offset = (int)(args[0] - (uintptr_t)bits);
        int quotient = offset / pitch;
        int found = 0;
        for (int y = quotient - 1; y <= quotient + 1; ++y) {
            int remaining = offset - y * pitch;
            int x = remaining / 2;
            if (y >= 0 && y + h <= SCENE_HEIGHT && remaining >= 0 && !(remaining & 1) &&
                remaining < abs(pitch) && x + w <= SCENE_WIDTH) {
                SetRect(&g_work->ships[g_current].box, x, y, x + w, y + h);
                found = 1;
                break;
            }
        }
        if (!found) {
            g_work->valid = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_work->valid = 0;
    }
    double cost = (double)(ticks() - start);
    g_cost += cost;
    g_step_cost += cost;
    SetLastError(error);
}

static void __stdcall observe_step(const DWORD *saved)
{
    (void)saved;
    ++g_step;
}

static void __stdcall observe_blend(const DWORD *saved)
{
    (void)saved;
    if (g_open) {
        g_work->valid = 0;
    }
}

/* ECX=view; arguments are frame and render-camera x/y. Observation only.
 * Failure keeps C-B available as the cloud fallback. */
static void __stdcall observe_cloud_enter(const DWORD *saved)
{
    DWORD error = GetLastError();
    LONGLONG start = ticks();
    __try {
        const DWORD *args = saved + 11;
        if (!g_lock || !g_open || !g_work->valid || (BYTE *)args[0] != g_frame) {
            __leave;
        }
        g_work->cloud_flags = *(BYTE *)CLOUD_FLAGS_ADDRESS;
        if (!copy_layer(SCENE_D)) {
            __leave;
        }
        BYTE *view = (BYTE *)saved[6];
        for (unsigned i = 0; i < SCENE_CLOUDS; ++i) {
            BYTE *state = view + CLOUD_STATE + i * CLOUD_STRIDE;
            scene_cloud *out = &g_work->clouds[i];
            out->image = *(BYTE *)(state - 8);
            out->ticks = *(unsigned *)(state + 8);
            out->x = *(double *)(state + 0x10);
            out->y = *(double *)(state + 0x18);
            out->vx = *(double *)(state + 0x20);
            out->vy = *(double *)(state + 0x28);
            if (out->image >= 18 ||
                !(fabs(out->x) < 1e8 && fabs(out->y) < 1e8 && fabs(out->vx) < 1e8 && fabs(out->vy) < 1e8)) {
                continue;
            }
            BYTE *bitmap = view + ((g_work->cloud_flags & 2) ? CLOUD_BLEND_BITMAP : CLOUD_KEY_BITMAP) +
                           out->image * CLOUD_BITMAP_STRIDE;
            out->width = *(int *)(bitmap + DIB_WIDTH);
            out->height = *(int *)(bitmap + DIB_HEIGHT);
            out->dest.x = (int)(out->x < 0 ? ceil(out->x - .5) : floor(out->x + .5)) - (int)args[1];
            out->dest.y = (int)(out->y < 0 ? ceil(out->y - .5) : floor(out->y + .5)) - (int)args[2];
            out->valid = out->width > 0 && out->width <= 2048 && out->height > 0 && out->height <= 2048;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_work->layers &= ~(1u << SCENE_D);
    }
    double cost = (double)(ticks() - start);
    g_cost += cost;
    g_step_cost += cost;
    SetLastError(error);
}

static void __stdcall observe_cloud_leave(const DWORD *saved)
{
    (void)saved;
    DWORD error = GetLastError();
    LONGLONG start = ticks();
    __try {
        if (g_lock && g_open && (g_work->layers & (1u << SCENE_D))) {
            copy_layer(SCENE_E);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_work->layers &= ~(1u << SCENE_E);
    }
    double cost = (double)(ticks() - start);
    g_cost += cost;
    g_step_cost += cost;
    SetLastError(error);
}

/* Bridges preserve flags, all GPRs, x87/SSE and exact ESP. The saved image
 * remains above aligned 512-byte FXSAVE storage and its saved-image pointer. */
// clang-format off
#define OBSERVE_BRIDGE(name, observer) \
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
OBSERVE_BRIDGE(cloud_enter_bridge, observe_cloud_enter)
OBSERVE_BRIDGE(cloud_leave_bridge, observe_cloud_leave)
OBSERVE_BRIDGE(enter_bridge, observe_enter)
OBSERVE_BRIDGE(leave_bridge, observe_leave)
OBSERVE_BRIDGE(ship_bridge, observe_ship)
OBSERVE_BRIDGE(step_bridge, observe_step)
OBSERVE_BRIDGE(blend_bridge, observe_blend)
OBSERVE_BRIDGE(blit_bridge, observe_blit)

/* Pass entry: [ESP]=return, +4..+16=four args, ECX=ships.
 * The original call consumes four copied args; RET 16 consumes originals.
 * Post bridge preserves every original result register and flag. */
__declspec(naked) static void pass_thunk(void)
{
    __asm {
        call enter_bridge
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        call dword ptr [g_pass_fn]
        call leave_bridge
        ret 16
    }
}

/* Three copied arguments consumed by game; RET 12 consumes originals.
 * Bridges preserve ECX, result registers, flags and FP state. */
__declspec(naked) static void cloud_thunk(void)
{
    __asm {
        call cloud_enter_bridge
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        push dword ptr [esp + 12]
        call dword ptr [g_cloud_fn]
        call cloud_leave_bridge
        ret 12
    }
}

/* Tail thunks keep original return/arguments/ECX intact; original callee returns. */
__declspec(naked) static void ship_thunk(void)
{
    __asm {
        call ship_bridge
        jmp dword ptr [g_ship_fn]
    }
}

__declspec(naked) static void blend_thunk(void)
{
    __asm {
        call blend_bridge
        jmp dword ptr [g_blend_fn]
    }
}

__declspec(naked) static void blit_thunk(void)
{
    __asm {
        call blit_bridge
        jmp dword ptr [g_blit_fn]
    }
}

/* Step entry: unchanged stack. Replay exactly the displaced MOV EAX,FS:[0]
 * and PUSH -1, then resume the original SEH prologue (no game call here). */
__declspec(naked) static void step_thunk(void)
{
    __asm {
        call step_bridge
        mov eax, fs:[0]
        push -1
        jmp dword ptr [g_step_resume]
    }
}

// clang-format on

int scene_init(SRWLOCK *lock)
{
    if (!patch_check((void *)CLOUD_CALL_ADDRESS, CLOUD_BYTES, sizeof(CLOUD_BYTES)) ||
        !patch_check((void *)HULL_CALL_ADDRESS, HULL_BYTES, sizeof(HULL_BYTES)) ||
        !patch_check((void *)SHIP_PASS_CALL_ADDRESS, PASS_BYTES, sizeof(PASS_BYTES)) ||
        !patch_check((void *)SHIP_CALL_ADDRESS, SHIP_BYTES, sizeof(SHIP_BYTES)) ||
        !patch_check((void *)BLEND_CALL_ADDRESS, BLEND_BYTES, sizeof(BLEND_BYTES)) ||
        !patch_check((void *)BLIT_CALL_ADDRESS, BLIT_BYTES, sizeof(BLIT_BYTES)) ||
        !patch_check((void *)STEP_ADDRESS, STEP_BYTES, sizeof(STEP_BYTES))) {
        log_line("scene: unexpected hook bytes; plain frames only");
        return 0;
    }
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    g_frequency = frequency.QuadPart;
    g_last_log = GetTickCount();
    for (unsigned i = 0; i < SCENE_RING; ++i) {
        g_queue[i] = &g_storage[i];
    }
    /* Hooks remain inert until the complete set is installed. */
    if (!patch_call((void *)CLOUD_CALL_ADDRESS, CLOUD_BYTES, (void *)(uintptr_t)cloud_thunk) ||
        !patch_call((void *)HULL_CALL_ADDRESS, HULL_BYTES, (void *)(uintptr_t)pass_thunk) ||
        !patch_call((void *)SHIP_PASS_CALL_ADDRESS, PASS_BYTES, (void *)(uintptr_t)pass_thunk) ||
        !patch_call((void *)SHIP_CALL_ADDRESS, SHIP_BYTES, (void *)(uintptr_t)ship_thunk) ||
        !patch_call((void *)BLEND_CALL_ADDRESS, BLEND_BYTES, (void *)(uintptr_t)blend_thunk) ||
        !patch_call((void *)BLIT_CALL_ADDRESS, BLIT_BYTES, (void *)(uintptr_t)blit_thunk) ||
        !patch_jmp_padded((void *)STEP_ADDRESS, STEP_BYTES, sizeof(STEP_BYTES),
                          (void *)(uintptr_t)step_thunk)) {
        log_line("scene: hook installation failed; plain frames only");
        return 0;
    }
    g_lock = lock;
    log_line("scene: capture ready ring=%d", SCENE_RING);
    return 1;
}

void scene_present(HDC source, LONG epoch)
{
    if (!g_lock) {
        return;
    }
    LONGLONG start = ticks();
    __try {
        if (!g_open && mission() && g_had_record && g_step == g_last_step) {
            return;
        }
        if (!mission() || !g_open || *(HDC *)(g_frame + DIB_HDC) != source || !copy_layer(SCENE_C)) {
            g_work->valid = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_work->valid = 0;
    }
    g_work->valid = g_work->valid && (g_work->layers & ((1u << SCENE_D) - 1)) == (1u << SCENE_D) - 1;
    if (!g_work->valid) {
        ++g_failures;
    }
    g_open = 0;
    g_last_step = g_step;
    g_had_record = 1;
    g_work->epoch = epoch;
    AcquireSRWLockExclusive(g_lock);
    if (g_size == SCENE_RING) {
        g_head = (g_head + 1) % SCENE_RING;
        --g_size;
        ++g_overruns;
    }
    unsigned index = (g_head + g_size) % SCENE_RING;
    scene_record *free_record = g_queue[index];
    g_queue[index] = g_work;
    g_work = free_record;
    ++g_size;
    ++g_records;
    double cost = (double)(ticks() - start);
    g_cost += cost;
    g_step_cost += cost;
    g_cost_max = max(g_cost_max, g_step_cost);
    g_step_cost = 0;
    g_published_cost = g_cost;
    g_published_max = g_cost_max;
    g_published_failures = g_failures;
    ReleaseSRWLockExclusive(g_lock);
}

int scene_take(scene_record **record)
{
    if (!g_lock) {
        return 0;
    }
    AcquireSRWLockExclusive(g_lock);
    /* A stalled consumer needs the newest pair, not textures for endpoints
     * which would be evicted before its next draw. Keep two for interpolation. */
    while (g_size > 2) {
        g_head = (g_head + 1) % SCENE_RING;
        --g_size;
        ++g_coalesced;
    }
    if (!g_size) {
        ReleaseSRWLockExclusive(g_lock);
        return 0;
    }
    scene_record *old = *record;
    *record = g_queue[g_head];
    g_queue[g_head] = old;
    g_head = (g_head + 1) % SCENE_RING;
    --g_size;
    ReleaseSRWLockExclusive(g_lock);
    return 1;
}

void scene_capture_log(void)
{
    if (!g_lock) {
        return;
    }
    AcquireSRWLockExclusive(g_lock);
    unsigned records = g_records;
    unsigned failures = g_published_failures;
    unsigned overruns = g_overruns;
    unsigned coalesced = g_coalesced;
    double mean = records ? g_published_cost * 1000 / (double)g_frequency / records : 0;
    double maximum = g_published_max * 1000 / (double)g_frequency;
    ReleaseSRWLockExclusive(g_lock);
    static unsigned last_records;
    static unsigned last_failures;
    static unsigned last_overruns;
    static unsigned last_coalesced;
    DWORD now = GetTickCount();
    double seconds = (DWORD)(now - g_last_log) / 1000.0;
    log_line("scene: records_interval=%u records_s=%.2f invalid_interval=%u overruns_interval=%u "
             "coalesced_interval=%u",
             records - last_records, seconds ? (records - last_records) / seconds : 0,
             failures - last_failures, overruns - last_overruns, coalesced - last_coalesced);
    last_records = records;
    last_failures = failures;
    last_overruns = overruns;
    last_coalesced = coalesced;
    g_last_log = now;
    log_line("scene: records=%u invalid=%u overruns=%u capture_ms_mean=%.3f capture_ms_max=%.3f", records,
             failures, overruns, mean, maximum);
}
