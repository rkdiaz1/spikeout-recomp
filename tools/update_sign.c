/* The in-game update's signer (src/net/update_sign.h has the format; docs/updating.md the whole story).
 *
 *   update_sign keygen SECRETFILE   a new key pair: the secret (64 hex digits) into SECRETFILE, the public key printed
 *                                   (CMake's SPIKEOUT_UPDATE_KEY). Keep SECRETFILE (and a copy) off the repository:
 *                                   it goes into the GitHub secret SPIKEOUT_UPDATE_SECRET.
 *   update_sign sign FILE...        each FILE signed into FILE.sig, with the secret in SPIKEOUT_UPDATE_SECRET (the
 *                                   release workflow) or the file SPIKEOUT_UPDATE_SECRET_FILE names
 *   update_sign check KEY FILE...   each FILE.sig checked against the public key KEY (hex)
 *
 * Built with only a C compiler, as the release workflow does:
 *   cc -O2 -Ithird_party/monocypher -Isrc tools/update_sign.c third_party/monocypher/monocypher.c -o update_sign
 *   (Windows: add -lbcrypt) */
#include "monocypher.h"
#include "net/update_sign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

static int random_bytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE* f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return 0;
    got = fread(out, 1, n, f);
    fclose(f);
    return got == n;
#endif
}

static void print_hex(FILE* f, const uint8_t* p, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) fprintf(f, "%02x", p[i]);
}

static const char* base_name(const char* path) {
    const char* b = path;
    const char* p;
    for (p = path; *p; ++p)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* The message signed for a file (0: it can't be read, or its name can't be signed). */
static size_t message_of(const char* path, uint8_t msg[SPK_UPDATE_MESSAGE_MAX]) {
    static uint8_t buf[1 << 16];
    crypto_blake2b_ctx ctx;
    uint8_t hash[64];
    size_t n;
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "update_sign: can't read %s\n", path);
        return 0;
    }
    crypto_blake2b_init(&ctx, 64);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) crypto_blake2b_update(&ctx, buf, n);
    fclose(f);
    crypto_blake2b_final(&ctx, hash);
    n = spk_update_message(msg, base_name(path), hash);
    if (n == 0) fprintf(stderr, "update_sign: %s: a name that can't be signed\n", path);
    return n;
}

static int read_secret(uint8_t seed[32]) {
    char text[200] = {0};
    const char* hex = getenv("SPIKEOUT_UPDATE_SECRET");
    const char* file = getenv("SPIKEOUT_UPDATE_SECRET_FILE");
    size_t n;
    if ((!hex || !*hex) && file && *file) {
        FILE* f = fopen(file, "rb");
        if (!f) {
            fprintf(stderr, "update_sign: can't read %s\n", file);
            return 0;
        }
        n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
    } else if (hex) {
        n = strlen(hex);
        if (n > sizeof text - 1) n = sizeof text - 1;
        memcpy(text, hex, n);
    } else {
        n = 0;
    }
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ')) text[--n] = 0; /* (a line's end) */
    hex = text;
    if (!hex || !spk_update_unhex(seed, 32, hex)) {
        fprintf(stderr, "update_sign: no secret key (SPIKEOUT_UPDATE_SECRET: 64 hex digits)\n");
        crypto_wipe(text, sizeof text);
        return 0;
    }
    crypto_wipe(text, sizeof text);
    return 1;
}

int main(int argc, char** argv) {
    uint8_t seed[32], secret[64], pub[32], sig[64], msg[SPK_UPDATE_MESSAGE_MAX];
    int i, bad = 0;
    if (argc == 3 && strcmp(argv[1], "keygen") == 0) {
        FILE* f;
        if (!random_bytes(seed, 32)) {
            fprintf(stderr, "update_sign: no random bytes\n");
            return 1;
        }
        f = fopen(argv[2], "wx"); /* (never over a key already there) */
        if (!f) {
            fprintf(stderr, "update_sign: can't make %s (is one there already?)\n", argv[2]);
            return 1;
        }
        print_hex(f, seed, 32);
        fprintf(f, "\n");
        fclose(f);
        crypto_eddsa_key_pair(secret, pub, seed); /* (wipes the seed) */
        crypto_wipe(secret, sizeof secret);
        printf("public key: ");
        print_hex(stdout, pub, 32);
        printf("\n");
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "sign") == 0) {
        if (!read_secret(seed)) return 1;
        crypto_eddsa_key_pair(secret, pub, seed);
        for (i = 2; i < argc; ++i) {
            char sig_path[4096];
            FILE* f;
            const size_t n = message_of(argv[i], msg);
            if (n == 0) {
                bad = 1;
                continue;
            }
            crypto_eddsa_sign(sig, secret, msg, n);
            snprintf(sig_path, sizeof sig_path, "%s.sig", argv[i]);
            f = fopen(sig_path, "wb");
            if (!f || fwrite(sig, 1, 64, f) != 64) {
                fprintf(stderr, "update_sign: can't write %s\n", sig_path);
                bad = 1;
            } else {
                printf("signed %s (key ", base_name(argv[i]));
                print_hex(stdout, pub, 32);
                printf(")\n");
            }
            if (f) fclose(f);
        }
        crypto_wipe(secret, sizeof secret);
        return bad;
    }
    if (argc >= 4 && strcmp(argv[1], "check") == 0) {
        if (!spk_update_unhex(pub, 32, argv[2])) {
            fprintf(stderr, "update_sign: the key must be 64 hex digits\n");
            return 1;
        }
        for (i = 3; i < argc; ++i) {
            char sig_path[4096];
            FILE* f;
            size_t n = message_of(argv[i], msg);
            snprintf(sig_path, sizeof sig_path, "%s.sig", argv[i]);
            f = fopen(sig_path, "rb");
            if (n == 0 || !f || fread(sig, 1, 64, f) != 64 || crypto_eddsa_check(sig, pub, msg, n) != 0) {
                printf("BAD %s\n", argv[i]);
                bad = 1;
            } else {
                printf("good %s\n", argv[i]);
            }
            if (f) fclose(f);
        }
        return bad;
    }
    fprintf(stderr, "usage: update_sign keygen SECRETFILE | sign FILE... | check KEY FILE...\n");
    return 2;
}
