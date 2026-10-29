#include "gun_smoke.h"
#include "patch.h"
#include "log.h"

/* Successful ship shot: skip smoke when ship counter % 5 != 0. */
#define SMOKE_REMAINDER_BRANCH 0x45e1ef

static const uint8_t SMOKE_BRANCH_BYTES[] = {0x75, 0x50};
static const uint8_t SMOKE_EVERY_SHOT_BYTES[] = {0x90, 0x90};

/* Called only after main.c accepts the original or CD-patched .text SHA-256. */
void gun_smoke_apply(int every_shot)
{
    if (!every_shot) {
        log_line("gun_smoke: original shot thinning");
        return;
    }

    /* Keep the game's smoke option (0x20), factory and offscreen rejection. */
    if (!patch_write((void *)SMOKE_REMAINDER_BRANCH, SMOKE_BRANCH_BYTES, SMOKE_EVERY_SHOT_BYTES,
                     sizeof(SMOKE_BRANCH_BYTES))) {
        log_line("gun_smoke: skipped (unexpected bytes or patch write failed)");
        return;
    }

    log_line("gun_smoke: every successful shot enabled");
}
