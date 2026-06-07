#ifndef OTA_TRANSFER_H_
#define OTA_TRANSFER_H_

/*
 * OTA aktarim protokolu - paylasilan tanimlar.
 * Gonderici (udp-client.c, dugum 2) ve Alici (udp-server.c, dugum 1) ortak kullanir.
 *
 * Butunluk icin CRC-32 (polinom 0xEDB88320, yansimali/reflected) kullanilir.
 * Bu, ota-metadata.c icindeki ota_crc32_buffer() ile ayni algoritmadir.
 */

#include <stdint.h>
#include "ota-metadata.h"   /* ota_crc32_buffer() icin */

/* Her veri blogunun yararli yuk boyutu (bayt). Tek 802.15.4 cercevesine sigsin diye kucuk. */
#define OTA_DATA_BLOCK_SIZE   32

/* Alicida yeniden birlestirilen imajin yazildigi kalici dosya (Coffee/CFS). */
#define OTA_FILENAME          "fw_b.bin"

/* Paket tipleri (her paketin ilk bayti) */
#define OTA_PKT_INIT       1   /* gonderici -> alici: aktarim basligi */
#define OTA_PKT_DATA       2   /* gonderici -> alici: bir veri blogu */
#define OTA_PKT_DONE       3   /* gonderici -> alici: bitti bildirimi */
#define OTA_PKT_ACK_INIT  11   /* alici -> gonderici: INIT onayi */
#define OTA_PKT_ACK_DATA  12   /* alici -> gonderici: blok onayi (seq) */

/* ---- Kucuk-uclu (little-endian) byte yaz/oku yardimcilari ---- */
static inline void
ota_put_u16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void
ota_put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static inline uint16_t
ota_get_u16(const uint8_t *p)
{
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t
ota_get_u32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- Artimli (streaming) CRC-32 ----
 * Tum imaji RAM'e sigdiramadigimiz icin, alici blok blok geldikce CRC'yi gunceller.
 * init -> update(her blok) -> final  zinciri, butun baytlar uzerinden tek
 * ota_crc32_buffer() cagrisiyla AYNI sonucu verir.
 */
static inline uint32_t
ota_crc32_init(void)
{
  return 0xFFFFFFFFu;
}
static inline uint32_t
ota_crc32_update(uint32_t crc, const void *buf, unsigned len)
{
  const uint8_t *p = (const uint8_t *)buf;
  unsigned i;
  int bit;
  for(i = 0; i < len; i++) {
    crc ^= p[i];
    for(bit = 0; bit < 8; bit++) {
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
  }
  return crc;
}
static inline uint32_t
ota_crc32_final(uint32_t crc)
{
  return ~crc;
}

#endif /* OTA_TRANSFER_H_ */
