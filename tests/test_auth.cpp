#include <cstdio>
#include <cstdlib>
#include "ble_auth.h"
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
  uint8_t psk[32], other[32], nD[16], nN[16];
  for (int i = 0; i < 32; i++) { psk[i] = i * 7 + 1; other[i] = i * 3 + 9; }
  for (int i = 0; i < 16; i++) { nD[i] = 0xA0 + i; nN[i] = 0x50 + i; }

  // handshake: both sides compute the same proofs and session key
  uint8_t pN_node[32], pN_disp[32], pD_disp[32], pD_node[32], ks_d[32], ks_n[32], bad[32];
  ba_derive(psk, 'N', nD, nN, pN_node); ba_derive(psk, 'N', nD, nN, pN_disp);
  CHECK(ba_equal(pN_node, pN_disp, AUTH_PROOF_LEN));
  ba_derive(psk, 'D', nN, nD, pD_disp); ba_derive(psk, 'D', nN, nD, pD_node);
  CHECK(ba_equal(pD_disp, pD_node, AUTH_PROOF_LEN));
  CHECK(!ba_equal(pN_node, pD_disp, AUTH_PROOF_LEN));           // proofs are direction-bound
  ba_derive(other, 'N', nD, nN, bad);
  CHECK(!ba_equal(bad, pN_disp, AUTH_PROOF_LEN));               // wrong key fails
  ba_derive(psk, 'S', nD, nN, ks_d); ba_derive(psk, 'S', nD, nN, ks_n);
  CHECK(ba_equal(ks_d, ks_n, 32));

  // control frame round trip
  BoilerControl c = { 455, 0, CF_CH_ENABLE | CF_DHW_ENABLE }, c2;
  uint8_t f[sizeof(c) + sizeof(FrameTrailer)];
  memcpy(f, &c, sizeof(c));
  size_t n = ba_seal(ks_d, FRAME_CTRL, 1, f, sizeof(c));
  CHECK(n == sizeof(f));
  CHECK(memcmp(f, &c, sizeof(c)) != 0);                         // actually encrypted
  uint32_t last = 0;
  CHECK(ba_open(ks_n, FRAME_CTRL, f, n, sizeof(c), &last, (uint8_t *)&c2));
  CHECK(memcmp(&c, &c2, sizeof(c)) == 0 && last == 1);
  CHECK(!ba_open(ks_n, FRAME_CTRL, f, n, sizeof(c), &last, (uint8_t *)&c2));      // replay
  last = 0;
  CHECK(!ba_open(ks_n, FRAME_CMD, f, n, sizeof(c), &last, (uint8_t *)&c2));       // wrong type
  f[1] ^= 0x01;
  CHECK(!ba_open(ks_n, FRAME_CTRL, f, n, sizeof(c), &last, (uint8_t *)&c2));      // tampered ciphertext
  f[1] ^= 0x01; f[n - 1] ^= 0x80;
  CHECK(!ba_open(ks_n, FRAME_CTRL, f, n, sizeof(c), &last, (uint8_t *)&c2));      // tampered tag
  f[n - 1] ^= 0x80;
  uint8_t ks_bad[32]; ba_derive(other, 'S', nD, nN, ks_bad);
  CHECK(!ba_open(ks_bad, FRAME_CTRL, f, n, sizeof(c), &last, (uint8_t *)&c2));    // wrong session key
  CHECK(ba_open(ks_n, FRAME_CTRL, f, n, sizeof(c), &last, (uint8_t *)&c2));       // untouched frame still fine
  CHECK(!ba_open(ks_n, FRAME_CTRL, f, n - 1, sizeof(c), &last, (uint8_t *)&c2));  // wrong length

  // status frame, larger payload, counters keep increasing
  BoilerStatus s = {}, s2; s.flow_c10 = 581; s.flags = BF_FLAME | BF_OT_LINK; s.flags2 = BF2_CLAMPED; s.seq = 7;
  uint8_t g[sizeof(s) + sizeof(FrameTrailer)]; uint32_t rx = 0;
  for (uint32_t k = 1; k <= 3; k++) {
    memcpy(g, &s, sizeof(s));
    CHECK(ba_seal(ks_n, FRAME_STATUS, k, g, sizeof(s)) == sizeof(g));
    CHECK(ba_open(ks_d, FRAME_STATUS, g, sizeof(g), sizeof(s), &rx, (uint8_t *)&s2));
    CHECK(memcmp(&s, &s2, sizeof(s)) == 0);
  }
  CHECK(rx == 3);
  CHECK(sizeof(BoilerStatus) == 21 && sizeof(BoilerControl) == 5 && sizeof(FrameTrailer) == 12);

  printf(fails ? "%d FAILED\n" : "all protocol tests passed\n", fails);
  return fails ? 1 : 0;
}
