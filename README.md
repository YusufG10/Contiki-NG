# BİL 304 — OTA Firmware Aktarımı (Geliştirme İş Parçacığı)

Contiki-NG + Cooja üzerinde, kablosuz (UDP/6LoWPAN/RPL) ortamda bir firmware imajının **parçalanarak**, **sırayla**, **hata denetimiyle** ve **yeniden gönderim** mekanizmasıyla bir düğümden diğerine aktarılması; alıcıda **kalıcı depolamaya (Coffee/CFS)** yazılıp **CRC-32 ile bütünlüğünün** doğrulanması.

## 🎥 Demo Videosu
> Buraya YouTube linkini koy: `https://youtu.be/...`

## Topoloji ve Roller

| Düğüm | Kaynak | Rol |
|-------|--------|-----|
| 1 | `udp-server.c` | **Alıcı** (DAG kökü). Blokları toplar, doğrular, Coffee'ye yazar, bütünlüğü onaylar. |
| 2 | `udp-client.c` | **Gönderici**. Firmware'i bloklara bölüp stop-and-wait ile yollar. |
| 3 | `udp-client.c` | **Röle** (`node_id != 2`). Göndermez, yalnızca yönlendirmeye yardım eder, menzili uzatır. |

Hedef platform **Z1 (MSP430F2617)**. Z1'in ~92 KB flash sınırı nedeniyle, gönderici motoya 127 KB'lık `new-firmware.z1`'in tamamı sığmadığından, gerçek imajın **ilk 4096 baytı** temsilî olarak gönderilir (`firmware-data.h`). Protokol boyuttan bağımsızdır; gerçek donanımda imaj harici bellekten akıtılır.

## Gerçeklenen Yöntemler

### 1. Parçalama (chunking)
Firmware, sabit **32 baytlık** bloklara bölünür. 4096 bayt → **128 blok**. Her bloğa bir **sıra numarası (seq)** atanır.

### 2. Paket biçimleri ve uzunlukları
Tüm sayısal alanlar küçük-uçlu (little-endian). Her paketin ilk baytı tiptir.

| Paket | Yön | Alanlar | Uzunluk |
|-------|-----|---------|---------|
| `INIT` (1) | G→A | tip(1) + image_size(4) + total_blocks(2) + block_size(2) + image_crc32(4) | **13 bayt** |
| `DATA` (2) | G→A | tip(1) + seq(2) + len(2) + block_crc32(4) + veri(≤32) | **≤ 41 bayt** |
| `DONE` (3) | G→A | tip(1) | **1 bayt** |
| `ACK_INIT` (11) | A→G | tip(1) | **1 bayt** |
| `ACK_DATA` (12) | A→G | tip(1) + seq(2) | **3 bayt** |

Blok yükü 32 bayt seçilerek tüm paketler tek bir IEEE 802.15.4 çerçevesine (127 bayt) sığar; 6LoWPAN parçalanması (fragmentation) önlenir.

### 3. Güvenilir aktarım — Stop-and-Wait + yeniden gönderim
Gönderici bir bloğu yollar ve o bloğun `ACK`'ini bekler. `RTX_TIMEOUT` (2 sn) içinde ACK gelmezse blok **yeniden gönderilir** (en çok `MAX_RETRIES` kez). ACK gelince bir sonraki bloğa geçilir. Bu, kayıp/çakışan paketlere karşı dayanıklılık sağlar.

### 4. Sıralı birleştirme
Alıcı yalnızca **beklenen sıradaki** bloğu (`seq == expected_seq`) kabul eder, sırayla Coffee dosyasına yazar ve `expected_seq`'i artırır. Tekrar gelen (duplicate) blok yeniden onaylanır; sıra dışı blok yok sayılır. Böylece imaj **doğru sırada** yeniden oluşur.

### 5. Kalıcı depolama (Coffee / CFS)
Alınan her blok, alıcıda `cfs_write()` ile **`fw_b.bin`** dosyasına eklenir. INIT'te dosya `cfs_open(CFS_WRITE)` ile sıfırdan açılır. Bu, yeniden birleştirilen imajın uçucu olmayan depolamada (flash) saklandığını gösterir.

### 6. Bütünlük denetimi — CRC-32
- **Blok bazında:** Her `DATA` paketinde o bloğun CRC-32'si taşınır. Alıcı CRC'yi yeniden hesaplar; tutmazsa paketi **düşürür** → gönderici ACK alamayıp bloğu yeniden gönderir.
- **Tüm imaj:** Gönderici INIT'te tüm imajın CRC-32'sini bildirir. Alıcı, blokları aldıkça **artımlı (streaming) CRC-32** günceller; son blokta hesabı tamamlayıp gönderilenle karşılaştırır. Eşitse imaj **doğrulanmış** sayılır, `ota-metadata` ile slot B "verified/pending" olarak işaretlenir ve tamamlanma mesajı basılır.

## Alınan Önlemler (özet)
- **Blok CRC-32** → bozuk blokların tespiti ve reddi.
- **Stop-and-wait + zaman aşımıyla yeniden gönderim** → kayıp paketlerin kurtarılması.
- **Sıra numarası + sıralı kabul** → yanlış sıralamayı engelleme.
- **Duplicate yeniden-ACK** → tekrar gelen paketlerde kilitlenmeyi önleme.
- **Tüm-imaj CRC-32** → yeniden birleştirmenin doğruluğunu uçtan uca garanti.
- **A/B slot meta verisi** → yeni imaj "aday" işaretlenir, aktif imaj korunur (güvenli güncelleme).

## Kullanılan Hash/Bütünlük Algoritması: CRC-32

**CRC (Cyclic Redundancy Check)**, veriyi GF(2) üzerinde büyük bir polinom olarak görüp sabit bir **üretici polinoma** bölen, kalanı sağlama değeri olarak kullanan bir hata-tespit yöntemidir. Bu projede **yansımalı (reflected) CRC-32**, üretici polinom **0xEDB88320** (standart Ethernet/ZIP/PNG CRC-32'sinin yansıması) kullanılır.

- 32 bitlik çıktı üretir.
- Tüm tek-bit ve çift-bit hatalarını, tek sayıda bit hatasını ve 32 bite kadar **patlama (burst) hatalarını** kesin yakalar.
- Tablosuz, bit-bit uygulanır (`ota-transfer.h` ve `ota-metadata.c`).
- **Önemli ayrım:** CRC-32 *kazara* bozulmaları tespit eder; *kötü niyetli kurcalamaya* karşı değildir. Kriptografik bütünlük için SHA-256/HMAC gerekir. Bu projede amaç iletişim hatalarını yakalamak olduğundan CRC-32 yeterli ve hızlıdır.

```c
/* Artımlı CRC-32 (ota-transfer.h) — tüm imajı RAM'e sığdırmadan hesaplar */
static inline uint32_t ota_crc32_update(uint32_t crc, const void *buf, unsigned len) {
  const uint8_t *p = buf;
  for(unsigned i = 0; i < len; i++) {
    crc ^= p[i];
    for(int b = 0; b < 8; b++)
      crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
  }
  return crc;
}
```

## Önemli Kod Parçaları

**Gönderici — parçalama + stop-and-wait (udp-client.c):**
```c
for(seq = 0; seq < total_blocks; seq++) {
  off = seq * OTA_DATA_BLOCK_SIZE;
  len = (FW_IMAGE_SIZE - off >= OTA_DATA_BLOCK_SIZE) ? OTA_DATA_BLOCK_SIZE : (FW_IMAGE_SIZE - off);
  block_crc = ota_crc32_buffer(&firmware_image[off], len);
  /* paketi kur: tip|seq|len|crc|veri */ ...
  ack_data = 0;
  for(tries = 0; tries < MAX_RETRIES; tries++) {
    simple_udp_sendto(&udp_conn, buf, 9 + len, &dst);
    etimer_set(&rtx, RTX_TIMEOUT);
    PROCESS_WAIT_EVENT_UNTIL((ack_data && ack_seq == seq) || etimer_expired(&rtx));
    if(ack_data && ack_seq == seq) break;     /* onaylandı */
    /* yoksa yeniden gönder */
  }
}
```

**Alıcı — doğrula, sırayla yaz, bütünlük (udp-server.c):**
```c
if(ota_crc32_buffer(payload, len) != bcrc) return;   /* bozuk blok: düşür */
if(seq == expected_seq) {
  cfs_write(fd, payload, len);                        /* Coffee'ye yaz */
  running_crc = ota_crc32_update(running_crc, payload, len);
  expected_seq++;
  send_ack_data(seq, from);
  if(expected_seq == total_blocks) {
    if(ota_crc32_final(running_crc) == image_crc32)
      printf("Yuklenmeye hazir yeni firmware alimi tamamlandi.\n");
  }
}
```

## Simülasyon Sonucu (Cooja, Log Listener)
```
ID:3  Bu dugum (id=3) role gorevinde, gonderim yapmaz.
ID:2  Kok erisilebilir. OTA aktarimi basliyor.
ID:2  INIT gonderiliyor (boyut=4096, blok=128, crc=0xdb96bd94) deneme 1
ID:1  OTA INIT alindi: boyut=4096, blok=128, beklenen CRC=0xdb96bd94
ID:1  Blok 0/128 ... 16/128 ... 112/128 alindi
ID:1  Alim bitti: 4096 bayt, hesaplanan CRC=0xdb96bd94, beklenen=0xdb96bd94
ID:1  Butunluk DOGRULANDI. 'fw_b.bin' dosyasina yazildi, slot B hazir.
ID:1  Yuklenmeye hazir yeni firmware alimi tamamlandi.
ID:2  Tum 128 blok gonderildi ve onaylandi. Gonderim tamam.
```
Hesaplanan ve beklenen CRC-32 (`0xdb96bd94`) birebir eşleşmiş; imaj eksiksiz ve doğru sırada yeniden birleştirilmiştir.

## Çalıştırma
```bash
# Proje: contiki-ng/examples/bil304-ota/
cd ~/contiki-ng/tools/cooja && ./gradlew run
# Cooja: File -> Open simulation -> BIL304-OS-Project-1.csc -> Start
```
Gereken araç: MSP430-GCC 4.7.2 (Z1 derlemesi için).

## Dosyalar
- `udp-client.c` — gönderici (düğüm 2) ve röle (düğüm 3)
- `udp-server.c` — alıcı / DAG kökü (düğüm 1)
- `ota-transfer.h` — paket tipleri, byte yardımcıları, artımlı CRC-32
- `ota-metadata.c/.h` — A/B slot meta verisi ve CRC-32 (şablon)
- `firmware-data.h` — gerçek `new-firmware.z1`'in ilk 4 KB'lık temsilî dilimi
- `BIL304-OS-Project-1.csc` — Cooja simülasyon yapılandırması
