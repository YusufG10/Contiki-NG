#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "cfs/cfs.h"

#include "ota-metadata.h"
#include "ota-transfer.h"

#include <stdint.h>

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

static struct simple_udp_connection udp_conn;

/* A/B slot acilis meta verisi (sablondan gelen yapi) */
static ota_boot_metadata_t boot_metadata = {
  .magic = OTA_IMAGE_MAGIC,
  .active_slot = OTA_SLOT_A,
  .candidate_slot = OTA_SLOT_NONE,
  .state_a = OTA_IMAGE_STATE_CONFIRMED,
  .state_b = OTA_IMAGE_STATE_EMPTY,
};

/* Alim durumu */
static int      fd = -1;
static uint16_t expected_seq = 0;
static uint16_t total_blocks = 0;
static uint32_t image_size = 0;
static uint32_t image_crc32 = 0;
static uint32_t running_crc = 0;     /* artimli tum-imaj CRC */
static uint32_t bytes_written = 0;

/*---------------------------------------------------------------------------*/
PROCESS(udp_server_process, "UDP server (OTA alici)");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
static void
send_ack_init(const uip_ipaddr_t *to)
{
  uint8_t b = OTA_PKT_ACK_INIT;
  simple_udp_sendto(&udp_conn, &b, 1, to);
}
static void
send_ack_data(uint16_t seq, const uip_ipaddr_t *to)
{
  uint8_t b[3];
  b[0] = OTA_PKT_ACK_DATA;
  ota_put_u16(&b[1], seq);
  simple_udp_sendto(&udp_conn, b, 3, to);
}
/*---------------------------------------------------------------------------*/
static void
handle_init(const uint8_t *data, uint16_t datalen, const uip_ipaddr_t *from)
{
  if(datalen < 13) {
    return;
  }
  image_size   = ota_get_u32(&data[1]);
  total_blocks = ota_get_u16(&data[5]);
  /* data[7..8] = blok boyutu (bilgi amacli) */
  image_crc32  = ota_get_u32(&data[9]);

  if(fd >= 0) {
    cfs_close(fd);
    fd = -1;
  }
  cfs_remove(OTA_FILENAME);
  fd = cfs_open(OTA_FILENAME, CFS_WRITE);
  if(fd < 0) {
    LOG_WARN("CFS/Coffee acilamadi! Yine de CRC ile dogrulama yapilacak.\n");
  }

  expected_seq  = 0;
  bytes_written = 0;
  running_crc   = ota_crc32_init();

  LOG_INFO("OTA INIT alindi: boyut=%lu, blok=%u, beklenen CRC=0x%08lx\n",
           (unsigned long)image_size, total_blocks, (unsigned long)image_crc32);
  send_ack_init(from);
}
/*---------------------------------------------------------------------------*/
static void
handle_data(const uint8_t *data, uint16_t datalen, const uip_ipaddr_t *from)
{
  uint16_t seq, len;
  uint32_t bcrc;
  const uint8_t *payload;

  if(datalen < 9) {
    return;
  }
  seq     = ota_get_u16(&data[1]);
  len     = ota_get_u16(&data[3]);
  bcrc    = ota_get_u32(&data[5]);
  payload = &data[9];

  if(datalen < (uint16_t)(9 + len)) {
    return;   /* bozuk/eksik paket */
  }

  /* Blok bazli hata kontrolu: CRC tutmuyorsa paketi dusur -> gonderici
     ACK alamaz, zaman asiminda blogu yeniden gonderir. */
  if(ota_crc32_buffer(payload, len) != bcrc) {
    LOG_WARN("Blok %u CRC hatasi, dusuruldu (yeniden istenecek)\n", seq);
    return;
  }

  if(seq == expected_seq) {
    if(fd >= 0) {
      cfs_write(fd, payload, len);   /* sirayla geldigi icin dogrudan yaz */
    }
    running_crc = ota_crc32_update(running_crc, payload, len);
    bytes_written += len;
    expected_seq++;
    send_ack_data(seq, from);

    if((seq % 16) == 0) {
      LOG_INFO("Blok %u/%u alindi (%lu bayt)\n",
               seq, total_blocks, (unsigned long)bytes_written);
    }

    /* Tum bloklar geldi mi? */
    if(total_blocks > 0 && expected_seq == total_blocks) {
      uint32_t final_crc;
      if(fd >= 0) {
        cfs_close(fd);
        fd = -1;
      }
      final_crc = ota_crc32_final(running_crc);
      LOG_INFO("Alim bitti: %lu bayt, hesaplanan CRC=0x%08lx, beklenen=0x%08lx\n",
               (unsigned long)bytes_written, (unsigned long)final_crc,
               (unsigned long)image_crc32);

      if(final_crc == image_crc32) {
        ota_metadata_mark_verified(&boot_metadata, OTA_SLOT_B,
                                   /*version*/ 2u, image_size, final_crc);
        ota_metadata_stage_verified_image(&boot_metadata, OTA_SLOT_B);
        LOG_INFO("Butunluk DOGRULANDI. '%s' dosyasina yazildi, slot B hazir.\n",
                 OTA_FILENAME);
        LOG_INFO("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
      } else {
        LOG_WARN("Butunluk HATASI! CRC uyusmadi, imaj reddedildi.\n");
      }
    }
  } else if(seq < expected_seq) {
    /* Tekrar gelen (duplicate) blok: yeniden onayla, gonderici ilerlesin */
    send_ack_data(seq, from);
  }
  /* seq > expected: stop-and-wait'te beklenmez; sessizce yok say */
}
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  (void)c; (void)sender_port; (void)receiver_addr; (void)receiver_port;

  if(datalen < 1) {
    return;
  }
  switch(data[0]) {
  case OTA_PKT_INIT:
    handle_init(data, datalen, sender_addr);
    break;
  case OTA_PKT_DATA:
    handle_data(data, datalen, sender_addr);
    break;
  case OTA_PKT_DONE:
    LOG_INFO("Gonderici DONE bildirdi.\n");
    break;
  default:
    break;
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  /* DAG kokunu baslat */
  NETSTACK_ROUTING.root_start();

  /* UDP baglantisini ac */
  simple_udp_register(&udp_conn, UDP_SERVER_PORT, NULL,
                      UDP_CLIENT_PORT, udp_rx_callback);

  LOG_INFO("OTA alici hazir (kok dugum). Gonderici bekleniyor...\n");

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
