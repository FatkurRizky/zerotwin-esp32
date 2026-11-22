# ZeroTwin ESP32 — evil twin + deauther + tombol fisik + monitor HP

Port dari [ZeroTwin](https://github.com/shinyxn) (ESP8266) ke **ESP32 DEVKIT V1** —
board yang sama dengan proyek smart pet feeder. Kontrol dan monitoring penuh dari
browser HP, tombol fisik tetap jalan, tanpa reboot otomatis.

> **Peringatan:** memalsukan AP, memutus koneksi WiFi, dan menangkap kata sandi
> milik jaringan orang lain adalah tindakan ilegal. Pakai hanya di perangkat /
> jaringan milik sendiri.

## Beda dengan versi ESP8266

| Aspek | ESP8266 | ESP32 (ini) |
| :--- | :--- | :--- |
| Injeksi frame | `wifi_send_pkt_freedom()` (`user_interface.h`) | `esp_wifi_80211_tx()` (`esp_wifi.h`), interface STA + promiscuous |
| Isi frame deauth | 26 byte `C0 00 … 07 00` | sama, byte-identik |
| Tombol | ADC `A0` | digital GPIO 18/19/21/22/23 (default) atau ADC `GPIO34` |
| OLED | wajib ada di sketch | opsional (`#define USE_OLED 1`) |
| RAM / IRAM | IRAM 93% (mepek) | lega, RAM puluhan KB sisa |

Deauth dikirim tiap 1 detik: frame `0xC0` (deauth) + `0xA0` (disassoc) ke BSSID
target, lalu kanal dikembalikan ke kanal AP kontrol supaya panel HP tidak ikut
putus.

## Wiring

### Tombol (default, digital)

| Fungsi | GPIO | Sambungan |
| :--- | :--- | :--- |
| UP — target berikutnya | 18 | kaki tombol → GPIO, kaki lain → GND |
| DOWN — scan ulang | 19 | idem |
| OK — start/stop deauth | 21 | idem |
| BACK — start/stop evil-twin | 22 | idem |
| EXTRA — stop semua | 23 | idem |

Semua `INPUT_PULLUP`, jadi tombol aktif LOW. Pin yang tidak dipasangi tombol
terbaca HIGH → **tidak ada aksi palsu** walau tombolnya belum ada.

Kalau mau satu pin ADC (rangkain resistor): set `#define BTN_ADC 1`, pakai
`GPIO34` (input-only) dengan ladder 3V3 → resistor → GND, dan sesuaikan ambang
di `readButton()` (nilai mentah tampil di panel sebagai `btn (raw)`).

### OLED (opsional)

`#define USE_OLED 1`, SDA `GPIO21`, SCL `GPIO22`, alamat `0x3C`.
**Konflik:** GPIO21/22 dipakai tombol OK/BACK di konfigurasi default — pindahkan
tombol ke pin lain (mis. 25/26/27) kalau memakai OLED.

## Build & flash

```bash
arduino-cli core install esp32:esp32      # diuji 4.0.0

cd zerotwin_esp32
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path ./build .
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32 --input-dir ./build
```

Board DEVKIT V1 pakai USB-UART CP2102/CH340; kalau unggah gagal, tahan tombol
`BOOT` saat proses upload mulai.

## Pakai dari HP

1. Sambung ke AP `ZeroTwin32` (password `zero8888`).
2. Buka `http://192.168.4.1` (captive portal biasanya membuka sendiri).
3. Pilih target di tabel → **Start Deauth** / **Start Evil-Twin**.
4. Baris status: uptime, heap, target, state, paket terkirim, tombol terakhir +
   nilai pin, password terakhir.

Ganti nama/password AP kontrol: panel bagian **AP Kontrol** → isi SSID/Password →
**Simpan AP**. Disimpan di `/zt32_ap.txt` (LittleFS), langsung diterapkan.

## Endpoint

```
GET  /                panel (atau halaman login saat twin aktif)
GET  /result          hasil percobaan password
GET  /api/status      up/heap/target/deauth/twin/pkt/btn/btnRaw/pass/ssid/aps
POST /?ap=N           pilih target (indeks tabel)
POST /?deauth=start|stop
POST /?hotspot=start|stop
POST /?stop=1         stop semua
POST /?setap=1&ssid=..&pass=..   ganti AP kontrol
POST /?password=..    percobaan password (saat twin aktif)
```

## Catatan teknis

- `esp_wifi_80211_tx()` butuh interface STA aktif dan promiscuous (dipanggil di
  `setup()`); kanal harus diset sebelum mengirim (`esp_wifi_set_channel`).
- ESP32 hanya punya satu radio: saat deauth pindah kanal, AP kontrol ikut pindah
  sesaat. Klien HP bisa terasa "kedip" — normal.
- Halaman login tiruan hanya untuk uji lab; password yang masuk disimpan di RAM
  dan dicoba ke AP asli lewat `WiFi.begin(ssid, pass, ch, bssid)`.
- Hasil kompilasi (core 4.0.0): lihat bagian bawah README ini setelah build.
