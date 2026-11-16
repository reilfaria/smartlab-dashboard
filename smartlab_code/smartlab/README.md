# SmartLab Lab APEL UNTIRTA

Arsitektur:

```
[ZH03B PM2.5] --UART(pin 8/10)--\
                                  >  Raspberry Pi (main.py) -- push --> Supabase --- baca ---> Dashboard (Vercel)
[DHT11 indoor] --GPIO4/pin7----/                ^
                                                 |
[DHT11 outdoor] --GPIO4-- ESP32 --WiFi/HTTP-----/
```

- **Raspi**: baca ZH03B + DHT11 indoor langsung, terima data DHT11 outdoor dari ESP32 via HTTP,
  lalu push semuanya ke Supabase.
- **Live data**: di-upsert ke tabel `live_readings` tiap 5 detik (dipakai dashboard untuk angka real-time).
- **Log histori**: disimpan ke tabel `sensor_logs` tiap 10 menit, sejak Raspi pertama kali jalan — inilah yang diunduh lewat tombol "Download CSV".
- **Dashboard**: file statis, baca langsung dari Supabase pakai anon key (read-only via RLS), di-deploy ke Vercel.

## 1. Setup Supabase
1. Buat project baru di supabase.com.
2. Buka **SQL Editor**, jalankan isi `supabase_schema.sql`.
3. Ambil dua kredensial dari **Project Settings > API**:
   - `Project URL`
   - `anon public key` → dipakai di dashboard (`dashboard/index.html`)
   - `service_role key` → dipakai di Raspi (`.env`), **jangan pernah** ditaruh di file dashboard/Vercel.

## 2. Setup Raspberry Pi
1. Aktifkan UART:
   ```
   sudo raspi-config
   → Interface Options → Serial Port
     - "Login shell over serial?" → No
     - "Serial port hardware enabled?" → Yes
   ```
   Kalau Raspi kamu punya Bluetooth on-board (Pi 3/4/Zero 2W) dan datanya sering korup,
   tambahkan `dtoverlay=disable-bt` di `/boot/firmware/config.txt` supaya UART0 penuh dipakai
   sensor (bukan mini-UART), lalu `sudo systemctl disable hciuart` dan reboot.
2. Cek ZH03B sudah dalam mode **aktif/kontinu** (bukan Q&A) — sebagian modul default-nya sudah aktif;
   kalau tidak ada data masuk sama sekali, cek datasheet ZH03B untuk perintah switch ke active mode.
3. Salin folder `raspberry_pi/` ke Raspi, lalu:
   ```
   cd raspberry_pi
   python3 -m venv venv && source venv/bin/activate
   pip install -r requirements.txt
   cp .env.example .env
   nano .env   # isi SUPABASE_URL, SUPABASE_SERVICE_KEY, ESP32_API_KEY, dll
   python3 main.py
   ```
4. Kalau sudah jalan lancar, pasang sebagai service biar otomatis nyala saat boot:
   ```
   sudo cp smartlab.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now smartlab.service
   journalctl -u smartlab -f   # buat lihat log
   ```
5. Catat IP lokal Raspi (`hostname -I`) — dipakai di sketch ESP32.

## 3. Setup ESP32
1. Buka `esp32/esp32_dht11_outdoor.ino` di Arduino IDE.
2. Install library "DHT sensor library" + "Adafruit Unified Sensor" (Library Manager).
3. Isi `WIFI_SSID`, `WIFI_PASSWORD`, `RASPI_URL` (pakai IP Raspi dari langkah sebelumnya),
   dan `API_KEY` (harus sama persis dengan `ESP32_API_KEY` di `.env` Raspi).
4. Upload ke ESP32. Buka Serial Monitor untuk pastikan WiFi konek dan POST sukses (HTTP 200).
5. ESP32 dan Raspi wajib berada di **jaringan WiFi/LAN yang sama**.

## 4. Setup Dashboard + Deploy Vercel
1. Buka `dashboard/index.html`, isi 3 baris konfigurasi di bagian atas `<script>`:
   ```js
   const SUPABASE_URL = "https://xxxxx.supabase.co";
   const SUPABASE_ANON_KEY = "anon-public-key-kamu";
   const DEVICE_ID = "raspi-apel-01";
   ```
   (Pakai **anon key**, bukan service key — anon key memang aman untuk dipasang di frontend karena
   dibatasi RLS yang sudah di-set read-only.)
2. Taruh folder `dashboard/` (beserta `assets/untirta.webp` logo kamu) sebagai root project di Vercel,
   lalu deploy sebagai static site — tidak perlu build step apa pun karena murni HTML/CSS/JS.
3. Setelah live, dashboard akan otomatis:
   - Polling `live_readings` tiap 5 detik untuk angka real-time & status online/offline (offline kalau
     tidak ada update > 30 detik).
   - Menarik 144 baris terakhir dari `sensor_logs` (≈24 jam, karena 1 baris/10 menit) untuk grafik.
   - Tombol **Download CSV** menarik SELURUH isi `sensor_logs` sejak awal sensor jalan, dengan kolom
     hari, tanggal, waktu, timestamp ISO, dan nilai tiap sensor.

## Catatan
- Kalau mau update lebih instan dari polling 5 detik, tabel `live_readings` bisa diaktifkan
  Supabase Realtime (Database → Replication) dan diganti pakai `sb.channel(...).on('postgres_changes', ...)`
  di `index.html`.
- Endpoint `/outdoor` di Raspi dilindungi header `X-API-KEY` sederhana. Karena ini cuma jalan di LAN lokal,
  itu cukup untuk kasus ini — tapi jangan expose port 5000 Raspi ke internet publik.
