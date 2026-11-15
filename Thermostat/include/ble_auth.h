// Crypto helpers for the BLE link. Header-only, shared by both projects.
//   handshake : HMAC-SHA256 with the pre-shared key
//   frames    : AES-256-CCM (encrypt + authenticate) with the per-session key
#pragma once
#include <stdint.h>
#include <string.h>
#include "mbedtls/md.h"
#include "mbedtls/ccm.h"
#include "ble_protocol.h"

#define BA_MAX_PAYLOAD 64

static inline void ba_hmac(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, klen, msg, mlen, out);
}

// constant-time compare
static inline bool ba_equal(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

// out32 = HMAC(psk, label | version | a | b)
static inline void ba_derive(const uint8_t psk[32], char label, const uint8_t *a, const uint8_t *b, uint8_t out[32]) {
  uint8_t m[2 + 2 * AUTH_NONCE_LEN];
  m[0] = (uint8_t)label;
  m[1] = BLE_PROTO_VERSION;
  memcpy(m + 2, a, AUTH_NONCE_LEN);
  memcpy(m + 2 + AUTH_NONCE_LEN, b, AUTH_NONCE_LEN);
  ba_hmac(psk, 32, m, sizeof(m), out);
}

// 12-byte CCM nonce, unique per (session key, frame type, counter)
static inline void ba_nonce(uint8_t type, uint32_t counter, uint8_t iv[12]) {
  memset(iv, 0, 12);
  iv[0] = type;
  memcpy(iv + 1, &counter, 4);
  iv[5] = 'C'; iv[6] = 'V';
}

// Encrypt payload in place and append the trailer. buf needs room for len + sizeof(FrameTrailer).
// Returns the frame length, or 0 on failure.
static inline size_t ba_seal(const uint8_t ks[32], uint8_t type, uint32_t counter, uint8_t *buf, size_t len) {
  if (len == 0 || len > BA_MAX_PAYLOAD) return 0;
  uint8_t iv[12], out[BA_MAX_PAYLOAD];
  FrameTrailer t;
  t.counter = counter;
  ba_nonce(type, counter, iv);
  mbedtls_ccm_context c;
  mbedtls_ccm_init(&c);
  int rc = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, ks, 256);
  if (rc == 0) rc = mbedtls_ccm_encrypt_and_tag(&c, len, iv, sizeof(iv), &type, 1, buf, out, t.tag, FRAME_TAG_LEN);
  mbedtls_ccm_free(&c);
  if (rc != 0) return 0;
  memcpy(buf, out, len);
  memcpy(buf + len, &t, sizeof(t));
  return len + sizeof(t);
}

// Verify and decrypt a received frame into out (payloadLen bytes). Updates lastCounter on success.
static inline bool ba_open(const uint8_t ks[32], uint8_t type, const uint8_t *frame, size_t frameLen,
                           size_t payloadLen, uint32_t *lastCounter, uint8_t *out) {
  if (payloadLen == 0 || payloadLen > BA_MAX_PAYLOAD || frameLen != payloadLen + sizeof(FrameTrailer)) return false;
  FrameTrailer t;
  memcpy(&t, frame + payloadLen, sizeof(t));
  if (t.counter <= *lastCounter) return false;                // replay or reorder
  uint8_t iv[12], plain[BA_MAX_PAYLOAD];
  ba_nonce(type, t.counter, iv);
  mbedtls_ccm_context c;
  mbedtls_ccm_init(&c);
  int rc = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, ks, 256);
  if (rc == 0) rc = mbedtls_ccm_auth_decrypt(&c, payloadLen, iv, sizeof(iv), &type, 1, frame, plain, t.tag, FRAME_TAG_LEN);
  mbedtls_ccm_free(&c);
  if (rc != 0) return false;
  memcpy(out, plain, payloadLen);
  *lastCounter = t.counter;
  return true;
}
