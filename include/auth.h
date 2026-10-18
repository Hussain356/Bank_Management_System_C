/*
 * auth.h - Password policy, salted + iterated hashing, constant-time verify.
 *
 * NOTE: This is built on SHA-256 so the project has zero dependencies. For a
 * real production system use a vetted KDF (Argon2id / scrypt / bcrypt / PBKDF2).
 */
#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTH_SALT_BYTES      16
#define AUTH_HASH_BYTES      32
#define AUTH_KDF_ITERATIONS  50000
#define AUTH_PASSWORD_MIN    8
#define AUTH_PASSWORD_MAX    64

typedef struct {
    uint8_t salt[AUTH_SALT_BYTES];
    uint8_t hash[AUTH_HASH_BYTES];
} Credential;

/* Fill buf with cryptographically secure random bytes. */
bool auth_random_bytes(uint8_t *buf, size_t n);

/*
 * Check a candidate password against the policy (8-64 chars, at least one
 * upper-case letter, one lower-case letter and one digit, no whitespace).
 * On failure writes a human readable reason into `reason`.
 */
bool auth_password_acceptable(const char *password, char *reason, size_t reason_size);

/* Create a fresh random salt and hash `password` into `out`. */
bool auth_make_credential(const char *password, Credential *out);

/* Constant-time verification of `password` against a stored credential. */
bool auth_verify(const char *password, const Credential *cred);

/* Constant-time memory comparison. */
bool auth_ct_equal(const uint8_t *a, const uint8_t *b, size_t n);

/* Overwrite sensitive memory in a way the compiler will not optimise away. */
void auth_wipe(void *p, size_t n);

#endif /* AUTH_H */
