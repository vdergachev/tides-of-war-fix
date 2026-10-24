#include "game.h"
#include "patch.h"

static const uint8_t CSTRING_CTOR_BYTES[] = {0xff, 0x25, 0x60, 0x02, 0x4a, 0x00};

/* ECX=output CString, EDX unused, text on stack; callee pops text. */
typedef void(__fastcall *cstring_ctor_fn)(void *, void *, const char *);

int game_cstring_check(void)
{
    return patch_check((void *)CSTRING_CTOR_ADDRESS, CSTRING_CTOR_BYTES, sizeof(CSTRING_CTOR_BYTES));
}

void *game_cstring_construct(void *out, const char *text)
{
    ((cstring_ctor_fn)CSTRING_CTOR_ADDRESS)(out, NULL, text);
    return out;
}
