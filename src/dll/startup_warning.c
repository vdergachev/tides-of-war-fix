#include "startup_warning.h"
#include "patch.h"
#include "log.h"

/* Conditional branch and arguments of the Windows 95 warning. */
#define WARNING_BRANCH_ADDRESS  0x407725
#define WARNING_PUSHES_ADDRESS  0x407727
#define WARNING_MESSAGE_ADDRESS 0x40772b
static const uint8_t WARNING_BRANCH_BYTES[] = {0x74, 0x0e};
static const uint8_t WARNING_PUSHES_BYTES[] = {0x6a, 0x00, 0x6a, 0x00};
static const uint8_t WARNING_MESSAGE_BYTES[] = {0x68, 0x8c, 0x14, 0x4c, 0x00};

int startup_warning_apply(void)
{
    if (!patch_check((void *)WARNING_BRANCH_ADDRESS, WARNING_BRANCH_BYTES, sizeof(WARNING_BRANCH_BYTES)) ||
        !patch_check((void *)WARNING_PUSHES_ADDRESS, WARNING_PUSHES_BYTES, sizeof(WARNING_PUSHES_BYTES)) ||
        !patch_check((void *)WARNING_MESSAGE_ADDRESS, WARNING_MESSAGE_BYTES, sizeof(WARNING_MESSAGE_BYTES))) {
        log_line("startup_warning: failed: unexpected code bytes");
        return 0;
    }

    const uint8_t jump = 0xeb;
    if (!patch_write((void *)WARNING_BRANCH_ADDRESS, WARNING_BRANCH_BYTES, &jump, 1)) {
        log_line("startup_warning: failed: code patch failed");
        return 0;
    }

    log_line("startup_warning: ok");
    return 1;
}
