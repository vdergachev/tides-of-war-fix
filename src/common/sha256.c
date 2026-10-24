#include <windows.h>
#include <wincrypt.h>
#include "sha256.h"

static int begin(HCRYPTPROV *p, HCRYPTHASH *h)
{
    *p = 0;
    *h = 0;
    return CryptAcquireContextA(p, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
           CryptCreateHash(*p, CALG_SHA_256, 0, 0, h);
}

static int finish(HCRYPTPROV p, HCRYPTHASH h, int ok, char hex[65])
{
    BYTE digest[32];
    DWORD n = sizeof(digest);
    static const char digits[] = "0123456789abcdef";
    if (ok) {
        ok = CryptGetHashParam(h, HP_HASHVAL, digest, &n, 0) && n == 32;
    }
    if (ok) {
        for (unsigned i = 0; i < 32; ++i) {
            hex[i * 2] = digits[digest[i] >> 4];
            hex[i * 2 + 1] = digits[digest[i] & 15];
        }
        hex[64] = 0;
    }

    /* Release even a partially initialized CryptoAPI context. */
    if (h) {
        CryptDestroyHash(h);
    }
    if (p) {
        CryptReleaseContext(p, 0);
    }
    return ok;
}

int sha256_memory(const void *data, size_t size, char hex[65])
{
    HCRYPTHASH h;
    HCRYPTPROV p;
    int ok = begin(&p, &h);
    if (ok) {
        ok = size <= MAXDWORD && CryptHashData(h, (const BYTE *)data, (DWORD)size, 0);
    }
    return finish(p, h, ok, hex);
}

int sha256_file(const char *path, char hex[65])
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    HCRYPTHASH h;
    HCRYPTPROV p;
    int ok = begin(&p, &h);
    while (ok) {
        BYTE buf[16384];
        DWORD n;
        if (!ReadFile(f, buf, sizeof(buf), &n, NULL)) {
            ok = 0;
            break;
        }
        if (!n) {
            break;
        }
        ok = CryptHashData(h, buf, n, 0);
    }
    CloseHandle(f);
    return finish(p, h, ok, hex);
}
