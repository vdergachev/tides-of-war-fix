#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "iat.h"

static int image_range(DWORD size, DWORD rva, size_t length)
{
    return rva <= size && length <= size - rva;
}

static int image_string(BYTE *base, DWORD size, DWORD rva, const char *expected, int insensitive)
{
    size_t length = strlen(expected) + 1;
    if (!image_range(size, rva, length)) {
        return 0;
    }
    return insensitive ? _strnicmp((char *)base + rva, expected, length) == 0
                       : memcmp(base + rva, expected, length) == 0;
}

static DWORD *find_import_thunk(BYTE *base, DWORD size, IMAGE_IMPORT_DESCRIPTOR *descriptor,
                                const char *function_name)
{
    DWORD names = descriptor->OriginalFirstThunk;
    DWORD slots = descriptor->FirstThunk;
    if (!names || !slots) {
        return NULL;
    }

    for (;;) {
        if (!image_range(size, names, sizeof(DWORD)) || !image_range(size, slots, sizeof(DWORD))) {
            return NULL;
        }
        DWORD name = *(DWORD *)(base + names);
        if (!name) {
            return NULL;
        }
        if (!(name & IMAGE_ORDINAL_FLAG32) && image_range(size, name, sizeof(WORD)) &&
            image_string(base, size, name + sizeof(WORD), function_name, 0)) {
            return (DWORD *)(base + slots);
        }
        if (!image_range(size, names, 2 * sizeof(DWORD)) || !image_range(size, slots, 2 * sizeof(DWORD))) {
            return NULL;
        }
        names += sizeof(DWORD);
        slots += sizeof(DWORD);
    }
}

/* Every descriptor, thunk and import name must fit inside SizeOfImage. */
DWORD *iat_find_slot_in(HMODULE module, const char *dll_name, const char *function_name)
{
    BYTE *base = (BYTE *)module;
    if (!base) {
        return NULL;
    }
    __try {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x100000) {
            return NULL;
        }
        IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) {
            return NULL;
        }

        DWORD size = nt->OptionalHeader.SizeOfImage;
        if (!image_range(size, 0, sizeof(*dos)) || !image_range(size, (DWORD)dos->e_lfanew, sizeof(*nt))) {
            return NULL;
        }
        IMAGE_DATA_DIRECTORY imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!image_range(size, imports.VirtualAddress, imports.Size)) {
            return NULL;
        }

        DWORD offset = 0;
        while (imports.Size - offset >= sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
            IMAGE_IMPORT_DESCRIPTOR *descriptor =
                (IMAGE_IMPORT_DESCRIPTOR *)(base + imports.VirtualAddress + offset);
            if (!descriptor->Name) {
                return NULL;
            }
            if (image_string(base, size, descriptor->Name, dll_name, 1)) {
                return find_import_thunk(base, size, descriptor, function_name);
            }
            offset += sizeof(*descriptor);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return NULL;
}

DWORD *iat_find_slot(const char *dll_name, const char *function_name)
{
    return iat_find_slot_in(GetModuleHandleA(NULL), dll_name, function_name);
}

int iat_replace(DWORD *slot, void *expected, void *replacement)
{
    if (!slot || *slot != (DWORD)(uintptr_t)expected) {
        return IAT_UNKNOWN_STATE;
    }
    DWORD old;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old)) {
        return IAT_NOT_WRITABLE;
    }
    *slot = (DWORD)(uintptr_t)replacement;
    DWORD ignored;
    if (!VirtualProtect(slot, sizeof(*slot), old, &ignored)) {
        *slot = (DWORD)(uintptr_t)expected;
        VirtualProtect(slot, sizeof(*slot), old, &ignored);
        return IAT_NOT_RESTORED;
    }
    return IAT_OK;
}
