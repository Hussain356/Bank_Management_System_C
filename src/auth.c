/*
 * auth.c - Password policy and credential hashing.
 */
#ifdef _WIN32
#define _CRT_RAND_S
#endif

#include "auth.h"
#include "sha256.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void auth_wipe(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

bool auth_ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

bool auth_random_bytes(uint8_t *buf, size_t n)
{
#ifdef _WIN32
    size_t i = 0;
    while (i < n) {
        unsigned int v;
        size_t k;
        if (rand_s(&v) != 0) return false;
        for (k = 0; k < sizeof(v) && i < n; k++, i++) {
            buf[i] = (uint8_t)(v >> (8 * k));
        }
    }
    return true;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return false;
    got = fread(buf, 1, n, f);
    fclose(f);
    return got == n;
#endif
}

bool auth_password_acceptable(const char *password, char *reason, size_t reason_size)
{
    size_t len = strlen(password);
    bool upper = false, lower = false, digit = false;
    size_t i;

    if (len < AUTH_PASSWORD_MIN) {
        snprintf(reason, reason_size, "Password must be at least %d characters long.", AUTH_PASSWORD_MIN);
        return false;
    }
    if (len > AUTH_PASSWORD_MAX) {
        snprintf(reason, reason_size, "Password must be at most %d characters long.", AUTH_PASSWORD_MAX);
        return false;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)password[i];
        if (isspace(c)) {
            snprintf(reason, reason_size, "Password must not contain spaces.");
            return false;
        }
        if (isupper(c)) upper = true;
        else if (islower(c)) lower = true;
        else if (isdigit(c)) digit = true;
    }
    if (!upper || !lower || !digit) {
        snprintf(reason, reason_size,
                 "Password needs an upper-case letter, a lower-case letter and a digit.");
        return false;
    }
    return true;
}

/* hash_0 = H(salt || pw); hash_i = H(hash_{i-1} || salt || pw) */
static void derive(const uint8_t salt[AUTH_SALT_BYTES], const char *password,
                   uint8_t out[AUTH_HASH_BYTES])
{
    const uint8_t *pw = (const uint8_t *)password;
    size_t pwlen = strlen(password);
    Sha256Ctx ctx;
    int i;

    sha256_init(&ctx);
    sha256_update(&ctx, salt, AUTH_SALT_BYTES);
    sha256_update(&ctx, pw, pwlen);
    sha256_final(&ctx, out);

    for (i = 1; i < AUTH_KDF_ITERATIONS; i++) {
        sha256_init(&ctx);
        sha256_update(&ctx, out, AUTH_HASH_BYTES);
        sha256_update(&ctx, salt, AUTH_SALT_BYTES);
        sha256_update(&ctx, pw, pwlen);
        sha256_final(&ctx, out);
    }
}

bool auth_make_credential(const char *password, Credential *out)
{
    if (!auth_random_bytes(out->salt, AUTH_SALT_BYTES)) return false;
    derive(out->salt, password, out->hash);
    return true;
}

bool auth_verify(const char *password, const Credential *cred)
{
    uint8_t h[AUTH_HASH_BYTES];
    bool ok;
    derive(cred->salt, password, h);
    ok = auth_ct_equal(h, cred->hash, AUTH_HASH_BYTES);
    auth_wipe(h, sizeof(h));
    return ok;
}
