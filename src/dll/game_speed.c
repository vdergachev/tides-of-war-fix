#include "game_speed.h"
#include "patch.h"
#include "log.h"

/* Timer constructor: shared initial delay, both stores, and return sequence. */
#define TIMER_DELAY_ADDRESS    0x446884
#define TIMER_STORES_ADDRESS   0x4468bf
#define TIMER_EPILOGUE_ADDRESS 0x446921
static const uint8_t TIMER_DELAY_BYTES[] = {0xb9, 0x3e, 0x00, 0x00, 0x00};
static const uint8_t TIMER_STORES_BYTES[] = {0x89, 0x8e, 0xf4, 0x00, 0x00, 0x00,
                                             0x89, 0x8e, 0xf8, 0x00, 0x00, 0x00};
static const uint8_t TIMER_EPILOGUE_BYTES[] = {0x8b, 0xc6, 0x5e, 0xc2, 0x04, 0x00};

static int g_step_delay;
static unsigned g_applied_delay = 78;

unsigned game_speed_step_delay(void)
{
    return g_applied_delay;
}

/* Entry: ESI=object, [ESP]=saved ESI, [ESP+4]=return, [ESP+8]=argument.
 * Exit: EAX=object, ESI restored, return and argument popped as in the constructor. */
// clang-format off
__declspec(naked) static void game_speed_thunk(void)
{
    __asm {
        mov eax, dword ptr [g_step_delay]
        mov dword ptr [esi + 0F4h], eax
        mov eax, esi
        pop esi
        ret 4
    }
}

// clang-format on

int game_speed_apply(int percent)
{
    if (percent < 20 || percent > 200) {
        log_line("game_speed: failed: percent out of range 20..200");
        return 0;
    }
    if (percent == 100) {
        g_applied_delay = 62;
        log_line("game_speed: original pace");
        return 1;
    }

    if (!patch_check((void *)TIMER_DELAY_ADDRESS, TIMER_DELAY_BYTES, sizeof(TIMER_DELAY_BYTES)) ||
        !patch_check((void *)TIMER_STORES_ADDRESS, TIMER_STORES_BYTES, sizeof(TIMER_STORES_BYTES)) ||
        !patch_check((void *)TIMER_EPILOGUE_ADDRESS, TIMER_EPILOGUE_BYTES, sizeof(TIMER_EPILOGUE_BYTES))) {
        log_line("game_speed: failed: unexpected code bytes");
        return 0;
    }

    g_step_delay = (6200 + percent / 2) / percent;
    if (!patch_jmp_padded((void *)TIMER_EPILOGUE_ADDRESS, TIMER_EPILOGUE_BYTES, sizeof(TIMER_EPILOGUE_BYTES),
                          (void *)(uintptr_t)game_speed_thunk)) {
        log_line("game_speed: failed: code patch failed");
        return 0;
    }

    log_line("game_speed: percent %d, step delay %d ms (original 62)", percent, g_step_delay);
    g_applied_delay = (unsigned)g_step_delay;
    log_line("game_speed: ok");
    return 1;
}
