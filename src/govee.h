// Govee H5074 advert decoder. Plain C++, no Arduino, so test/test_govee.cpp can run it anywhere.
//
// Manufacturer data, including the 2-byte company id (0xEC88, sent little-endian as 88 EC), is 9 bytes:
//   [0]=0x88 [1]=0xEC [2]=0x00
//   [3..4] temperature, int16 little-endian, 1/100 degC (negative = two's complement)
//   [5..6] relative humidity, uint16 little-endian, 1/100 %
//   [7]    battery %
//   [8]    unused
// Cross-checked against the open decoders: govee-ble (Home Assistant) unpacks "<hHB" from the
// payload after the company id and its 0x00 byte, /100; Theengs Decoder reads the same offsets.
// The H5074 also sends iBeacon adverts (company 0x004C); those are ignored here.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <ctype.h>
#include <string.h>

struct GoveeReading {
  int16_t t100;   // degC * 100
  uint16_t h100;  // %RH * 100
  uint8_t bat;    // %
};

inline bool decodeH5074(const uint8_t* d, size_t len, GoveeReading& out) {
  if (d == nullptr || len != 9) return false;
  if (d[0] != 0x88 || d[1] != 0xEC || d[2] != 0x00) return false;
  int16_t t = (int16_t)(uint16_t)(d[3] | (d[4] << 8));
  uint16_t h = (uint16_t)(d[5] | (d[6] << 8));
  uint8_t b = d[7];
  // Plausibility: the H5074 is rated -20..60 degC. Anything far outside is a bad packet.
  if (t < -4000 || t > 8000) return false;
  if (h > 10000) return false;
  if (b > 100) return false;
  out.t100 = t;
  out.h100 = h;
  out.bat = b;
  return true;
}

// "a4:c1:38:12:ab:cd" -> "ABCD" (last 4 hex digits; matches the Govee_H5074_ABCD name).
inline bool idFromMac(const char* mac, char out[5]) {
  if (mac == nullptr) return false;
  char hex[13];
  size_t n = 0;
  for (const char* p = mac; *p && n < 12; ++p) {
    if (isxdigit((unsigned char)*p)) hex[n++] = (char)toupper((unsigned char)*p);
  }
  if (n != 12) return false;
  memcpy(out, hex + 8, 4);
  out[4] = 0;
  return true;
}

// Fixed-point to text: -252 -> "-2.52", 5 -> "0.05", 5820 -> "58.20".
inline void fmtHundredths(int32_t v, char* buf, size_t cap) {
  int32_t a = v < 0 ? -v : v;
  char tmp[16];
  int k = 0;
  if (v < 0) tmp[k++] = '-';
  // integer part
  char ip[12];
  int ipn = 0;
  int32_t whole = a / 100;
  do { ip[ipn++] = (char)('0' + whole % 10); whole /= 10; } while (whole > 0);
  while (ipn > 0) tmp[k++] = ip[--ipn];
  tmp[k++] = '.';
  tmp[k++] = (char)('0' + (a % 100) / 10);
  tmp[k++] = (char)('0' + a % 10);
  tmp[k] = 0;
  strncpy(buf, tmp, cap);
  if (cap) buf[cap - 1] = 0;
}
