/* The in-game update's signatures (docs/updating.md), in C so that tools/update_sign.c (the release workflow's
 * signer, built with only a C compiler) and the game (src/net/install.cpp) share it.
 *
 * A download spikeout-<version>-<platform>.<zip|tar.gz> is signed with the project's update key (Monocypher's
 * EdDSA: Ed25519's curve with BLAKE2b) over the message
 *     "SPKUPD1\n" <the download's file name> "\n" <BLAKE2b-512 of its bytes, 64 bytes>
 * and the 64-byte signature published beside it as <file name>.sig. The name is signed too, so a signed download
 * can't be offered as another version's or another system's. The secret key lives only in the GitHub secret
 * SPIKEOUT_UPDATE_SECRET (32 bytes, hex: the seed), the public key in the game (CMake SPIKEOUT_UPDATE_KEY). */
#ifndef SPIKEOUT_UPDATE_SIGN_H
#define SPIKEOUT_UPDATE_SIGN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SPK_UPDATE_MAGIC "SPKUPD1\n"
#define SPK_UPDATE_NAME_MAX 200
#define SPK_UPDATE_MESSAGE_MAX (8 + SPK_UPDATE_NAME_MAX + 1 + 64)

/* The message signed for a download named `name` whose BLAKE2b-512 is `hash`; its length (0: a name too long or
 * holding a line break). */
static inline size_t spk_update_message(uint8_t out[SPK_UPDATE_MESSAGE_MAX], const char* name, const uint8_t hash[64]) {
    const size_t n = strlen(name);
    if (n == 0 || n > SPK_UPDATE_NAME_MAX || memchr(name, '\n', n) != NULL) return 0;
    memcpy(out, SPK_UPDATE_MAGIC, 8);
    memcpy(out + 8, name, n);
    out[8 + n] = '\n';
    memcpy(out + 9 + n, hash, 64);
    return 9 + n + 64;
}

/* Hex digits into bytes: 1 if `hex` is exactly 2 * size of them. */
static inline int spk_update_unhex(uint8_t* out, size_t size, const char* hex) {
    size_t i;
    if (strlen(hex) != 2 * size) return 0;
    for (i = 0; i < 2 * size; ++i) {
        const char c = hex[i];
        const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return 0;
        if (i % 2 == 0) out[i / 2] = (uint8_t)(v << 4);
        else out[i / 2] = (uint8_t)(out[i / 2] | v);
    }
    return 1;
}

#endif
