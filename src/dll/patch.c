#include <windows.h>
#include <string.h>
#include "patch.h"

/* x86 near relative jump used at the five-byte patch site. */
#define JMP_REL32_OPCODE 0xe9

int patch_check(void *addr, const uint8_t *expected, size_t n)
{
    __try {
        return n && memcmp(addr, expected, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int patch_write(void *addr, const uint8_t *expected, const uint8_t *bytes, size_t n)
{
    if (!patch_check(addr, expected, n)) {
        return 0;
    }
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }

    memcpy(addr, bytes, n);
    DWORD ignored;
    int ok = VirtualProtect(addr, n, old, &ignored) != 0;
    if (ok && FlushInstructionCache(GetCurrentProcess(), addr, n)) {
        return 1;
    }

    /* Restore the original bytes if protection or cache synchronization failed. */
    if (VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &ignored)) {
        memcpy(addr, expected, n);
        VirtualProtect(addr, n, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), addr, n);
    }
    return 0;
}

int patch_jmp(void *site, const uint8_t *expected5, void *target)
{
    uint8_t bytes[5] = {JMP_REL32_OPCODE};
    uint32_t rel = (uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)site - 5;
    memcpy(bytes + 1, &rel, 4);
    return patch_write(site, expected5, bytes, 5);
}

int patch_call(void *site, const uint8_t *expected5, void *target)
{
    /* x86 near relative call at a verified five-byte call site. */
    uint8_t bytes[5] = {0xe8};
    uint32_t rel = (uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)site - 5;
    memcpy(bytes + 1, &rel, 4);
    return patch_write(site, expected5, bytes, 5);
}

int patch_jmp_padded(void *site, const uint8_t *expected, size_t n, void *target)
{
    if (n < 5) {
        return 0;
    }

    uint8_t *bytes = HeapAlloc(GetProcessHeap(), 0, n);
    if (!bytes) {
        return 0;
    }
    memset(bytes, 0x90, n);
    bytes[0] = JMP_REL32_OPCODE;
    uint32_t rel = (uint32_t)(uintptr_t)target - (uint32_t)(uintptr_t)site - 5;
    memcpy(bytes + 1, &rel, sizeof(rel));
    int ok = patch_write(site, expected, bytes, n);
    HeapFree(GetProcessHeap(), 0, bytes);
    return ok;
}
