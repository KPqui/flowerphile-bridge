// Host test for the H5074 decoder. Run: g++ -std=c++17 -Wall -Werror -o t test/test_govee.cpp && ./t
#include <cstdio>
#include <cstring>
#include "../src/govee.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
  GoveeReading r{};

  // Example from a decoded H5074 advert: temp 0x092F = 23.51 C, RH 0x1741 = 59.53 %, battery 0x64 = 100 %.
  const uint8_t a[] = {0x88, 0xEC, 0x00, 0x2F, 0x09, 0x41, 0x17, 0x64, 0x02};
  CHECK(decodeH5074(a, sizeof a, r));
  CHECK(r.t100 == 2351 && r.h100 == 5953 && r.bat == 100);

  // Below zero: -2.52 C = 0xFF04.
  const uint8_t b[] = {0x88, 0xEC, 0x00, 0x04, 0xFF, 0x78, 0x0C, 0x57, 0x00};
  CHECK(decodeH5074(b, sizeof b, r));
  CHECK(r.t100 == -252 && r.h100 == 3192 && r.bat == 87);

  // Wrong company id, wrong length, H5075-style 8-byte packet, iBeacon: all rejected.
  const uint8_t c[] = {0x4C, 0x00, 0x02, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00};
  CHECK(!decodeH5074(c, sizeof c, r));
  CHECK(!decodeH5074(a, 8, r));
  const uint8_t d[] = {0x88, 0xEC, 0x00, 0x03, 0x21, 0x5D, 0x64, 0x00};
  CHECK(!decodeH5074(d, sizeof d, r));
  CHECK(!decodeH5074(nullptr, 9, r));

  // Implausible values rejected (humidity 655.35 %, battery 200 %).
  const uint8_t e[] = {0x88, 0xEC, 0x00, 0x2F, 0x09, 0xFF, 0xFF, 0x64, 0x00};
  CHECK(!decodeH5074(e, sizeof e, r));
  const uint8_t f[] = {0x88, 0xEC, 0x00, 0x2F, 0x09, 0x41, 0x17, 0xC8, 0x00};
  CHECK(!decodeH5074(f, sizeof f, r));

  char id[5];
  CHECK(idFromMac("a4:c1:38:12:ab:cd", id) && strcmp(id, "ABCD") == 0);
  CHECK(idFromMac("E0:12:1D:61:0A:9F", id) && strcmp(id, "0A9F") == 0);
  CHECK(!idFromMac("a4:c1:38", id));

  char s[16];
  fmtHundredths(-252, s, sizeof s); CHECK(strcmp(s, "-2.52") == 0);
  fmtHundredths(-5, s, sizeof s);   CHECK(strcmp(s, "-0.05") == 0);
  fmtHundredths(5820, s, sizeof s); CHECK(strcmp(s, "58.20") == 0);
  fmtHundredths(0, s, sizeof s);    CHECK(strcmp(s, "0.00") == 0);

  if (fails) { printf("%d check(s) failed\n", fails); return 1; }
  printf("all decoder checks passed\n");
  return 0;
}
