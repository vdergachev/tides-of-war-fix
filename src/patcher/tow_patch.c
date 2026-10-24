#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../common/sha256.h"

typedef struct {
    BYTE *data;
    DWORD size;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_SECTION_HEADER *sections;
} Image;

static int range(DWORD off, DWORD n, DWORD size)
{
    return off <= size && n <= size - off;
}

static int align_up(DWORD value, DWORD alignment, DWORD *out)
{
    if (!alignment || (alignment & (alignment - 1))) {
        return 0;
    }
    DWORD extra = (alignment - value % alignment) % alignment;
    if (value > MAXDWORD - extra) {
        return 0;
    }
    *out = value + extra;
    return 1;
}

static int parse(Image *im)
{
    IMAGE_DOS_HEADER *dos;
    if (!range(0, sizeof(*dos), im->size)) {
        return 0;
    }
    dos = (IMAGE_DOS_HEADER *)im->data;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
        return 0;
    }

    /* Validate the PE headers before following file offsets. */
    DWORD off = (DWORD)dos->e_lfanew;
    if (!range(off, sizeof(IMAGE_NT_HEADERS32), im->size)) {
        return 0;
    }
    im->nt = (IMAGE_NT_HEADERS32 *)(im->data + off);
    if (im->nt->Signature != IMAGE_NT_SIGNATURE || im->nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        im->nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32) ||
        im->nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        im->nt->OptionalHeader.NumberOfRvaAndSizes < IMAGE_NUMBEROF_DIRECTORY_ENTRIES) {
        return 0;
    }

    DWORD table = off + sizeof(IMAGE_NT_HEADERS32);
    DWORD count = im->nt->FileHeader.NumberOfSections;
    if (!count || !range(table, count * sizeof(IMAGE_SECTION_HEADER), im->size)) {
        return 0;
    }
    im->sections = (IMAGE_SECTION_HEADER *)(im->data + table);
    if (im->nt->OptionalHeader.SizeOfHeaders > im->size) {
        return 0;
    }

    unsigned i;
    for (i = 0; i < count; ++i) {
        IMAGE_SECTION_HEADER *s = &im->sections[i];
        if (!range(s->PointerToRawData, s->SizeOfRawData, im->size) ||
            s->VirtualAddress > MAXDWORD - s->Misc.VirtualSize) {
            return 0;
        }
    }
    return 1;
}

static BYTE *rva(Image *im, DWORD va, DWORD n)
{
    if (range(va, n, im->nt->OptionalHeader.SizeOfHeaders) && range(va, n, im->size)) {
        return im->data + va;
    }

    unsigned i;
    for (i = 0; i < im->nt->FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER *s = &im->sections[i];
        if (va < s->VirtualAddress) {
            continue;
        }
        DWORD delta = va - s->VirtualAddress;
        if (range(delta, n, s->SizeOfRawData) && range(s->PointerToRawData, delta, im->size) &&
            range(s->PointerToRawData + delta, n, im->size)) {
            return im->data + s->PointerToRawData + delta;
        }
    }
    return NULL;
}

static int dll_name(Image *im, DWORD va)
{
    static const char name[] = "tow_fix.dll";
    BYTE *p = rva(im, va, sizeof(name));
    return p && _stricmp((char *)p, name) == 0;
}

static int import_is_terminator(const IMAGE_IMPORT_DESCRIPTOR *d)
{
    return !d->OriginalFirstThunk && !d->TimeDateStamp && !d->ForwarderChain && !d->Name && !d->FirstThunk;
}

static int import_is_valid(Image *im, const IMAGE_IMPORT_DESCRIPTOR *d)
{
    return d->Name && d->FirstThunk && rva(im, d->Name, 1) && rva(im, d->FirstThunk, 4);
}

/* Return -1 for malformed imports, otherwise the descriptor count. */
static int imports(Image *im, int *has_fix, IMAGE_IMPORT_DESCRIPTOR **table)
{
    IMAGE_DATA_DIRECTORY *dir = &im->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    *has_fix = 0;
    *table = NULL;
    if (!dir->VirtualAddress && !dir->Size) {
        return 0;
    }
    if (!dir->VirtualAddress || dir->Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        return -1;
    }
    DWORD i;
    for (i = 0; i < dir->Size / sizeof(IMAGE_IMPORT_DESCRIPTOR); ++i) {
        IMAGE_IMPORT_DESCRIPTOR *d;
        DWORD delta = i * sizeof(*d);
        if (dir->VirtualAddress > MAXDWORD - delta) {
            return -1;
        }
        d = (IMAGE_IMPORT_DESCRIPTOR *)rva(im, dir->VirtualAddress + delta, sizeof(*d));
        if (!d) {
            return -1;
        }
        if (!i) {
            *table = d;
        }
        if (import_is_terminator(d)) {
            return (int)i;
        }
        if (!import_is_valid(im, d)) {
            return -1;
        }
        if (dll_name(im, d->Name)) {
            *has_fix = 1;
        }
        if (*table + i != d) {
            return -1;
        }
    }
    return -1;
}

static int read_image(const char *path, Image *im)
{
    memset(im, 0, sizeof(*im));
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > 0x7fffffff) {
        CloseHandle(f);
        return 0;
    }
    im->size = (DWORD)size.QuadPart;
    im->data = (BYTE *)malloc(im->size);
    int ok;
    DWORD got;
    ok = im->data && ReadFile(f, im->data, im->size, &got, NULL) && got == im->size;
    CloseHandle(f);
    if (!ok) {
        free(im->data);
        im->data = NULL;
        return 0;
    }
    return 1;
}

static const char *accepted(const char *hash)
{
    if (!strcmp(hash, "4cd1a3ddad2007e5c09fd55b02922ff59fdd20434d95e433e6e85ce7cb327967")) {
        return "original";
    }
    if (!strcmp(hash, "9dcf3f69bf505208e9826e6b9a56bb2356e2a7794debdc3ac589c1fbfc0a8289")) {
        return "cd-patched";
    }
    return NULL;
}

static int valid_backup(const char *path)
{
    char hash[65];
    return sha256_file(path, hash) && accepted(hash);
}

static DWORD checksum(const BYTE *data, DWORD size, DWORD skip)
{
    uint64_t sum = 0;
    DWORD i;
    for (i = 0; i < size; i += 2) {
        unsigned word = 0;
        if (i < skip || i >= skip + 4) {
            word = data[i];
            if (i + 1 < size) {
                word |= (unsigned)data[i + 1] << 8;
            }
        }
        sum += word;
        sum = (sum & 0xffff) + (sum >> 16);
    }
    sum = (sum & 0xffff) + (sum >> 16);
    return (DWORD)sum + size;
}

/* On-disk offsets of the appended import section; preserve the existing PE layout. */
typedef struct {
    DWORD va;
    DWORD raw;
    DWORD int_off;
    DWORD iat_off;
    DWORD hint_off;
    DWORD name_off;
    DWORD virtual_size;
    DWORD raw_size;
    DWORD image_size;
} ImportLayout;

static int plan_import_section(Image *im, int count, ImportLayout *layout)
{
    IMAGE_OPTIONAL_HEADER32 *opt = &im->nt->OptionalHeader;
    DWORD first_raw = im->size;
    IMAGE_SECTION_HEADER *s;

    /* Require unused header space for one additional section. */
    DWORD header_end = (DWORD)((BYTE *)(im->sections + im->nt->FileHeader.NumberOfSections) - im->data);
    DWORD i;
    for (i = 0; i < im->nt->FileHeader.NumberOfSections; ++i) {
        s = &im->sections[i];
        if (s->SizeOfRawData && s->PointerToRawData < first_raw) {
            first_raw = s->PointerToRawData;
        }
    }
    if (header_end > first_raw || first_raw - header_end < sizeof(*s) ||
        !range(header_end, sizeof(*s), opt->SizeOfHeaders)) {
        return 0;
    }
    for (i = header_end; i < first_raw; ++i) {
        if (im->data[i]) {
            return 0;
        }
    }

    /* Append after all existing virtual and raw section data. */
    IMAGE_SECTION_HEADER *last = &im->sections[im->nt->FileHeader.NumberOfSections - 1];
    if (!align_up(last->VirtualAddress + last->Misc.VirtualSize, opt->SectionAlignment, &layout->va) ||
        !align_up(im->size, opt->FileAlignment, &layout->raw)) {
        return 0;
    }
    for (i = 0; i < im->nt->FileHeader.NumberOfSections; ++i) {
        s = &im->sections[i];
        if (s->VirtualAddress >= layout->va || s->SizeOfRawData > layout->va - s->VirtualAddress ||
            s->Misc.VirtualSize > layout->va - s->VirtualAddress) {
            return 0;
        }
    }

    /* Two descriptors and two terminated thunk arrays precede the names. */
    layout->int_off = ((DWORD)count + 2) * sizeof(IMAGE_IMPORT_DESCRIPTOR);
    layout->iat_off = layout->int_off + 8;
    layout->hint_off = layout->iat_off + 8;
    layout->name_off = (layout->hint_off + 2 + sizeof("TowFixImport") + 3) & ~3u;
    layout->virtual_size = layout->name_off + sizeof("tow_fix.dll");
    if (!align_up(layout->virtual_size, opt->FileAlignment, &layout->raw_size) ||
        layout->raw > 0x7fffffff - layout->raw_size || layout->va > MAXDWORD - layout->virtual_size ||
        !align_up(layout->va + layout->virtual_size, opt->SectionAlignment, &layout->image_size) ||
        opt->SizeOfInitializedData > MAXDWORD - layout->raw_size) {
        return 0;
    }
    return 1;
}

static void fill_import_section(BYTE *data, const ImportLayout *layout, int count,
                                const IMAGE_IMPORT_DESCRIPTOR *old)
{
    BYTE *section = data + layout->raw;
    if (count) {
        memcpy(section, old, (size_t)count * sizeof(*old));
    }

    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)section + count;
    d->OriginalFirstThunk = layout->va + layout->int_off;
    d->FirstThunk = layout->va + layout->iat_off;
    d->Name = layout->va + layout->name_off;

    DWORD *thunk = (DWORD *)(section + layout->int_off);
    thunk[0] = layout->va + layout->hint_off;
    thunk = (DWORD *)(section + layout->iat_off);
    thunk[0] = layout->va + layout->hint_off;
    memcpy(section + layout->hint_off + 2, "TowFixImport", sizeof("TowFixImport"));
    memcpy(section + layout->name_off, "tow_fix.dll", sizeof("tow_fix.dll"));
}

static void update_import_headers(Image *im, const ImportLayout *layout)
{
    IMAGE_SECTION_HEADER *s = &im->sections[im->nt->FileHeader.NumberOfSections];
    memset(s, 0, sizeof(*s));
    memcpy(s->Name, ".towimp", 7);
    s->Misc.VirtualSize = layout->virtual_size;
    s->VirtualAddress = layout->va;
    s->SizeOfRawData = layout->raw_size;
    s->PointerToRawData = layout->raw;
    s->Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
    ++im->nt->FileHeader.NumberOfSections;

    IMAGE_OPTIONAL_HEADER32 *opt = &im->nt->OptionalHeader;
    opt->SizeOfImage = layout->image_size;
    opt->SizeOfInitializedData += layout->raw_size;
    opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = layout->va;
    opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = layout->int_off;

    DWORD checksum_off = (DWORD)((BYTE *)&opt->CheckSum - im->data);
    opt->CheckSum = checksum(im->data, im->size, checksum_off);
}

static const char *add_import(Image *im)
{
    IMAGE_OPTIONAL_HEADER32 *opt = &im->nt->OptionalHeader;
    int has;
    IMAGE_IMPORT_DESCRIPTOR *old;
    int count = imports(im, &has, &old);
    if (count < 0 || has || im->nt->FileHeader.NumberOfSections == 0xffff) {
        return "Cannot append import section; PE layout unsupported.\n";
    }
    if (opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].VirtualAddress ||
        opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT].Size) {
        return "Bound imports are not supported.\nCannot append import section; PE layout unsupported.\n";
    }

    ImportLayout layout;
    if (!plan_import_section(im, count, &layout)) {
        return "Cannot append import section; PE layout unsupported.\n";
    }

    DWORD new_size = layout.raw + layout.raw_size;
    BYTE *data = (BYTE *)calloc(1, new_size);
    if (!data) {
        return "Cannot append import section; PE layout unsupported.\n";
    }
    memcpy(data, im->data, im->size);
    fill_import_section(data, &layout, count, old);

    free(im->data);
    im->data = data;
    im->size = new_size;
    if (!parse(im)) {
        return "Cannot append import section; PE layout unsupported.\n";
    }
    update_import_headers(im, &layout);
    return NULL;
}

static int write_image(const char *path, Image *im)
{
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int ok;
    DWORD n;
    ok = WriteFile(f, im->data, im->size, &n, NULL) && n == im->size && FlushFileBuffers(f);
    if (!CloseHandle(f)) {
        ok = 0;
    }
    if (!ok) {
        DeleteFileA(path);
    }
    return ok;
}

/* Operations collect diagnostics; only the front-ends perform presentation. */
typedef struct {
    int code;
    const char *message;
    const char *state;
    char hash[65];
    char installed_hash[65];
    char dll[MAX_PATH];
} PatchResult;

static PatchResult result_error(const char *message)
{
    PatchResult result = {0};
    result.code = 1;
    result.message = message;
    return result;
}

static PatchResult restore_backup(const char *exe, const char *backup, const char *tmp)
{
    if (!valid_backup(backup)) {
        return result_error("Backup missing or has an unrecognized hash.\n");
    }
    if (!CopyFileA(backup, tmp, TRUE)) {
        return result_error("Cannot create restore temporary file.\n");
    }
    if (!valid_backup(tmp) || !MoveFileExA(tmp, exe, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        return result_error("Restore failed.\n");
    }

    PatchResult result = {0};
    result.message = "Backup restored.\n";
    return result;
}

static const char *require_fix_dll(const char *exe, PatchResult *result)
{
    strcpy_s(result->dll, sizeof(result->dll), exe);
    char *slash = strrchr(result->dll, '\\');
    if (!slash || (size_t)(slash + 1 - result->dll) + sizeof("tow_fix.dll") > sizeof(result->dll)) {
        return "DLL path too long.\n";
    }
    strcpy_s(slash + 1, sizeof(result->dll) - (size_t)(slash + 1 - result->dll), "tow_fix.dll");

    DWORD attrs = GetFileAttributesA(result->dll);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return "tow_fix.dll must be next to the executable: ";
    }
    result->dll[0] = 0;
    return NULL;
}

static const char *ensure_backup(const char *exe, const char *backup)
{
    DWORD attrs = GetFileAttributesA(backup);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND || !CopyFileA(exe, backup, TRUE)) {
            return "Cannot create backup.\n";
        }
    }
    if (!valid_backup(backup)) {
        return "Backup has an unrecognized hash.\n";
    }
    return NULL;
}

static const char *install_patched_image(const char *exe, const char *tmp, Image *im, Image *verify,
                                         char hash[65])
{
    if (!write_image(tmp, im)) {
        return "Cannot create temporary image (existing .tmp is preserved).\n";
    }
    IMAGE_IMPORT_DESCRIPTOR *table;
    int has;
    if (!read_image(tmp, verify) || !parse(verify) || imports(verify, &has, &table) < 0 || !has ||
        verify->size != im->size || memcmp(verify->data, im->data, im->size) != 0 ||
        !sha256_memory(verify->data, verify->size, hash)) {
        DeleteFileA(tmp);
        return "Temporary image verification failed.\n";
    }

    if (!MoveFileExA(tmp, exe, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        return "Cannot replace executable.\n";
    }

    /* Reopen the installed file after verifying the exact temporary bytes. */
    free(verify->data);
    memset(verify, 0, sizeof(*verify));
    if (!read_image(exe, verify) || !parse(verify) || imports(verify, &has, &table) < 0 || !has ||
        !sha256_memory(verify->data, verify->size, hash)) {
        return "Final image verification failed; restore the backup.\n";
    }
    return NULL;
}

static PatchResult patch_executable(const char *exe, const char *backup, const char *tmp, int check)
{
    PatchResult result = result_error(NULL);
    int has = 0;
    Image im = {0};
    Image verify = {0};
    if (!read_image(exe, &im) || !sha256_memory(im.data, im.size, result.hash)) {
        result.message = "Cannot read or hash executable.\n";
        goto done;
    }

    const char *version = accepted(result.hash);
    IMAGE_IMPORT_DESCRIPTOR *table;
    int count = parse(&im) ? imports(&im, &has, &table) : -1;
    if (count < 0) {
        result.message = "Invalid or unsupported PE/import table.\n";
        goto done;
    }
    result.state = has ? "already patched" : version ? version : "unknown";
    if (check) {
        result.code = (version || has) ? 0 : 1;
        goto done;
    }
    result.message = require_fix_dll(exe, &result);
    if (result.message) {
        goto done;
    }

    if (has) {
        result.code = 0;
        goto done;
    }
    if (!version) {
        result.message = "Unrecognized executable; refusing to patch.\n";
        goto done;
    }
    result.message = add_import(&im);
    if (result.message) {
        goto done;
    }
    result.message = ensure_backup(exe, backup);
    if (result.message) {
        goto done;
    }
    result.message = install_patched_image(exe, tmp, &im, &verify, result.installed_hash);
    if (result.message) {
        goto done;
    }
    result.code = 0;
done:
    free(im.data);
    free(verify.data);
    return result;
}

static int console_result(const PatchResult *result)
{
    if (result->state) {
        printf("SHA-256: %s\nState: %s\n", result->hash, result->state);
    }
    if (result->message) {
        FILE *stream = result->code ? stderr : stdout;
        fputs(result->message, stream);
        if (result->dll[0] && !strcmp(result->message, "tow_fix.dll must be next to the executable: ")) {
            fprintf(stream, "%s\n", result->dll);
        }
    } else if (!result->code && result->installed_hash[0]) {
        printf("Patched successfully.\nSHA-256: %s\n", result->installed_hash);
    }
    return result->code;
}

static int executable_paths(const char *arg, char exe[MAX_PATH], char backup[MAX_PATH], char tmp[MAX_PATH])
{
    DWORD n = GetFullPathNameA(arg, MAX_PATH, exe, NULL);
    if (!n || n >= MAX_PATH || strlen(exe) + 5 > MAX_PATH) {
        return 0;
    }
    strcpy_s(backup, MAX_PATH, exe);
    strcat_s(backup, MAX_PATH, ".bak");
    strcpy_s(tmp, MAX_PATH, exe);
    strcat_s(tmp, MAX_PATH, ".tmp");
    return 1;
}

static int console_main(int argc, char **argv)
{
    const char *arg = "TOW.exe";
    int check = 0;
    int restore = 0;
    int index = 1;
    if (index < argc && !strcmp(argv[index], "--check")) {
        check = 1;
        ++index;
    } else if (index < argc && !strcmp(argv[index], "--restore")) {
        restore = 1;
        ++index;
    }
    if (index < argc) {
        arg = argv[index++];
    }
    if (index != argc || !strncmp(arg, "--", 2)) {
        fprintf(stderr, "Usage: tow_patch.exe [--check|--restore] [path\\to\\TOW.exe]\n");
        return 1;
    }

    char exe[MAX_PATH];
    char backup[MAX_PATH];
    char tmp[MAX_PATH];
    if (!executable_paths(arg, exe, backup, tmp)) {
        fprintf(stderr, "Executable path is too long or invalid.\n");
        return 1;
    }
    PatchResult result =
        restore ? restore_backup(exe, backup, tmp) : patch_executable(exe, backup, tmp, check);
    return console_result(&result);
}

static int dialog_box(const wchar_t *message, UINT flags)
{
    return MessageBoxW(NULL, message, L"Tides of War fix", flags);
}

static int dialog_result(const PatchResult *result, const wchar_t *success)
{
    if (!result->code) {
        dialog_box(success, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    wchar_t message[1024];
    wchar_t dll[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, result->message ? result->message : "Operation failed.", -1, message,
                        _countof(message));
    if (result->dll[0] && result->message &&
        !strcmp(result->message, "tow_fix.dll must be next to the executable: ")) {
        MultiByteToWideChar(CP_ACP, 0, result->dll, -1, dll, _countof(dll));
        wcscat_s(message, _countof(message), dll);
        wcscat_s(message, _countof(message), L".");
    }
    dialog_box(message, MB_OK | MB_ICONERROR);
    return 1;
}

static int dialog_unknown(const char *hash)
{
    wchar_t message[512];
    swprintf_s(message, _countof(message),
               L"This TOW.exe is not a supported version.\n\nSHA-256: %hs\n\n"
               L"Supported: the English 1998 release (original or CD-patched). "
               L"Please report this hash to the fix authors.",
               hash);
    dialog_box(message, MB_OK | MB_ICONERROR);
    return 1;
}

static int dialog_main(void)
{
    char module[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, module, sizeof(module));
    char *slash = n && n < sizeof(module) ? strrchr(module, '\\') : NULL;
    if (!slash || (size_t)(slash + 1 - module) + sizeof("TOW.exe") > sizeof(module)) {
        dialog_box(L"Executable path is too long or invalid.", MB_OK | MB_ICONERROR);
        return 1;
    }
    strcpy_s(slash + 1, sizeof(module) - (size_t)(slash + 1 - module), "TOW.exe");

    char exe[MAX_PATH];
    char backup[MAX_PATH];
    char tmp[MAX_PATH];
    if (!executable_paths(module, exe, backup, tmp)) {
        dialog_box(L"Executable path is too long or invalid.", MB_OK | MB_ICONERROR);
        return 1;
    }
    DWORD attrs = GetFileAttributesA(exe);
    if (attrs == INVALID_FILE_ATTRIBUTES &&
        (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
        dialog_box(L"TOW.exe was not found next to tow_patch.exe. Copy the fix files into the game folder.",
                   MB_OK | MB_ICONERROR);
        return 1;
    }

    PatchResult result = patch_executable(exe, backup, tmp, 1);
    /* Even a non-PE file has a useful hash for reporting an unknown version. */
    if (result.hash[0] && (!result.state || !strcmp(result.state, "unknown"))) {
        return dialog_unknown(result.hash);
    }
    if (result.code) {
        return dialog_result(&result, NULL);
    }
    if (!strcmp(result.state, "already patched")) {
        if (dialog_box(L"The fix is already installed.\n\nRemove it and restore the original "
                       L"TOW.exe from TOW.exe.bak?",
                       MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return 0;
        }
        result = restore_backup(exe, backup, tmp);
        return dialog_result(&result, L"Backup restored.");
    }

    wchar_t path[MAX_PATH];
    wchar_t prompt[MAX_PATH + 128];
    MultiByteToWideChar(CP_ACP, 0, exe, -1, path, _countof(path));
    swprintf_s(prompt, _countof(prompt),
               L"Install the Tides of War fix into %ls?\n\nA backup will be saved as TOW.exe.bak.", path);
    if (dialog_box(prompt, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return 0;
    }

    result = patch_executable(exe, backup, tmp, 0);
    return dialog_result(&result, L"The fix is installed. Start the game with TOW.exe as usual.\n\n"
                                  L"Settings: tow_fix.ini\nLog: tow_fix.log (next to TOW.exe)");
}

int main(int argc, char **argv)
{
    DWORD process;
    if (argc == 1 && GetConsoleProcessList(&process, 1) == 1) {
        FreeConsole();
        return dialog_main();
    }
    return console_main(argc, argv);
}
