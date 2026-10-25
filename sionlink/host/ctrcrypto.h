// The small amount of cryptography the 3DS's local wireless and Download Play
// need on the host: AES-128 (CTR, and CCM as 802.11 CCMP uses it), MD5 and
// SHA-1. Written from the FIPS 197 / RFC 1321 / FIPS 180 / RFC 3610
// descriptions; speed doesn't matter at these packet rates.

#ifndef CTRCRYPTO_H
#define CTRCRYPTO_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t rk[44];
} aes128;

void Aes128Init(aes128 *a, const uint8_t key[16]);
void Aes128Block(const aes128 *a, const uint8_t in[16], uint8_t out[16]);

// AES-CTR with a 128-bit big-endian counter (in place)
void Aes128Ctr(const uint8_t key[16], const uint8_t ctr[16], uint8_t *buf, size_t len);

// AES-CCM, 8-byte tag, 2-byte length field (13-byte nonce), as in CCMP.
// Encrypt: buf in place, tag out. Decrypt: buf in place, returns 0 if the
// tag matches.
void Aes128CcmEncrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                      size_t aad_len, uint8_t *buf, size_t len, uint8_t tag[8]);
int Aes128CcmDecrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                     size_t aad_len, uint8_t *buf, size_t len, const uint8_t tag[8]);

void Md5(const void *data, size_t len, uint8_t out[16]);
void Sha1(const void *data, size_t len, uint8_t out[20]);

#endif
