#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/node-id.h"

#include "ota-metadata.h"
#include "ota-transfer.h"
#include "firmware-data.h"

#include <string.h>

#include "sys/log.h"
#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define RTX_TIMEOUT   (CLOCK_SECOND * 2)   /* ACK beklerken yeniden gonderim suresi */
#define MAX_RETRIES   10                   /* bir blok icin azami deneme */

static struct simple_udp_connection udp_conn;

/* rx callback'in process'i uyandirmasi icin bayraklar */
static volatile uint8_t ack_init = 0;
static volatile uint8_t ack_data = 0;
static volatile uint16_t ack_seq = 0;

/*---------------------------------------------------------------------------*/
PROCESS(udp_client_process, "UDP client (OTA gonderici)");
AUTOSTART_PROCESSES(&udp_client_process);
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
  (void)c; (void)sender_addr; (void)sender_port;
  (void)receiver_addr; (void)receiver_port;

  if(datalen < 1) {
    return;
  }
  switch(data[0]) {
  case OTA_PKT_ACK_INIT:
    ack_init = 1;
    break;
  case OTA_PKT_ACK_DATA:
    if(datalen >= 3) {
      ack_seq = ota_get_u16(&data[1]);
      ack_data = 1;
    }
    break;
  default:
    break;
  }
  /* process'i hemen uyandir ki ACK'i beklemeden devam edebilsin */
  process_poll(&udp_client_process);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer wait_timer;
  static struct etimer rtx;
  static uip_ipaddr_t dst;
  static uint16_t seq;
  static uint16_t total_blocks;
  static uint16_t off;
  static uint16_t len;
  static uint32_t block_crc;
  static uint32_t image_crc;
  static uint8_t tries;
  static uint8_t buf[9 + OTA_DATA_BLOCK_SIZE];

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn, UDP_CLIENT_PORT, NULL,
                      UDP_SERVER_PORT, udp_rx_callback);

  /* Yalnizca dugum 2 gonderici. Dugum 3 (ve digerleri) yalnizca role gorevi
     yapar; bu islem onlarda sonlanir, yonlendirme netstack tarafindan surer. */
  if(node_id != 2) {
    LOG_INFO("Bu dugum (id=%u) role gorevinde, gonderim yapmaz.\n", node_id);
    PROCESS_EXIT();
  }

  /* Kok (alici) erisilebilir olana kadar bekle */
  etimer_set(&wait_timer, CLOCK_SECOND);
  while(!(NETSTACK_ROUTING.node_is_reachable() &&
          NETSTACK_ROUTING.get_root_ipaddr(&dst))) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&wait_timer));
    etimer_reset(&wait_timer);
  }
  LOG_INFO("Kok erisilebilir. OTA aktarimi basliyor.\n");

  total_blocks = (uint16_t)((FW_IMAGE_SIZE + OTA_DATA_BLOCK_SIZE - 1) / OTA_DATA_BLOCK_SIZE);
  image_crc = ota_crc32_buffer(firmware_image, FW_IMAGE_SIZE);

  /* ----- 1) INIT el sikismasi (boyut, blok sayisi, tum-imaj CRC) ----- */
  buf[0] = OTA_PKT_INIT;
  ota_put_u32(&buf[1], (uint32_t)FW_IMAGE_SIZE);
  ota_put_u16(&buf[5], total_blocks);
  ota_put_u16(&buf[7], OTA_DATA_BLOCK_SIZE);
  ota_put_u32(&buf[9], image_crc);

  ack_init = 0;
  for(tries = 0; tries < MAX_RETRIES; tries++) {
    LOG_INFO("INIT gonderiliyor (boyut=%u, blok=%u, crc=0x%08lx) deneme %u\n",
             (unsigned)FW_IMAGE_SIZE, total_blocks,
             (unsigned long)image_crc, (unsigned)(tries + 1));
    simple_udp_sendto(&udp_conn, buf, 13, &dst);
    etimer_set(&rtx, RTX_TIMEOUT);
    PROCESS_WAIT_EVENT_UNTIL(ack_init || etimer_expired(&rtx));
    if(ack_init) {
      break;
    }
  }
  if(!ack_init) {
    LOG_WARN("INIT onayi alinamadi, aktarim iptal.\n");
    PROCESS_EXIT();
  }

  /* ----- 2) Veri bloklari: stop-and-wait + yeniden gonderim ----- */
  for(seq = 0; seq < total_blocks; seq++) {
    off = (uint16_t)(seq * OTA_DATA_BLOCK_SIZE);
    len = (uint16_t)((FW_IMAGE_SIZE - off) >= OTA_DATA_BLOCK_SIZE
                     ? OTA_DATA_BLOCK_SIZE : (FW_IMAGE_SIZE - off));
    block_crc = ota_crc32_buffer(&firmware_image[off], len);

    buf[0] = OTA_PKT_DATA;
    ota_put_u16(&buf[1], seq);
    ota_put_u16(&buf[3], len);
    ota_put_u32(&buf[5], block_crc);
    memcpy(&buf[9], &firmware_image[off], len);

    ack_data = 0;
    for(tries = 0; tries < MAX_RETRIES; tries++) {
      simple_udp_sendto(&udp_conn, buf, (uint16_t)(9 + len), &dst);
      etimer_set(&rtx, RTX_TIMEOUT);
      PROCESS_WAIT_EVENT_UNTIL((ack_data && ack_seq == seq) || etimer_expired(&rtx));
      if(ack_data && ack_seq == seq) {
        break;
      }
      LOG_INFO("Blok %u icin yeniden gonderim (deneme %u)\n",
               seq, (unsigned)(tries + 2));
    }
    if(!(ack_data && ack_seq == seq)) {
      LOG_WARN("Blok %u gonderilemedi, aktarim iptal.\n", seq);
      PROCESS_EXIT();
    }
    if((seq % 16) == 0) {
      LOG_INFO("Blok %u/%u gonderildi+onaylandi\n", seq, total_blocks);
    }
  }

  /* ----- 3) Bitti ----- */
  buf[0] = OTA_PKT_DONE;
  simple_udp_sendto(&udp_conn, buf, 1, &dst);
  LOG_INFO("Tum %u blok gonderildi ve onaylandi. Gonderim tamam.\n", total_blocks);

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
