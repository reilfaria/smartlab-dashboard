#!/bin/bash
# ============================================================
#  Update SmartLab Raspi: ENS160 (eCO2/TVOC/AQI) + watchdog anti-offline
#  Jalankan di Raspi dari folder mana saja:  bash update_raspi.sh
# ============================================================
set -e
DIR="$HOME/smartlab"
USR="$(whoami)"
cd "$DIR"
echo "== Folder: $DIR | user: $USR"

[ -f .env ] || echo "!! PERINGATAN: $DIR/.env tidak ditemukan, service tidak akan bisa jalan."
PY="/usr/bin/python3"
[ -x "$DIR/venv/bin/python" ] && PY="$DIR/venv/bin/python"
echo "== Python: $PY"

# 1) backup main.py lama
cp main.py "main.py.bak.$(date +%Y%m%d-%H%M)" 2>/dev/null && echo "== Backup main.py lama dibuat"

# 2) main.py baru
cat > main.py <<'SMARTLAB_MAIN_EOF'
"""
SmartLab Lab APEL UNTIRTA - Raspberry Pi collector (versi 2 ESP32)
====================================================================
Arsitektur:
  ESP32 #1 (outdoor): DHT11 luar             -> POST /outdoor {temp_out, hum_out}
  ESP32 #2 (indoor) : ZH03B (PM2.5) + ENS160/AHT21
                       -> POST /indoor  {pm25, temp_in, hum_in, eco2, tvoc, aqi, ens_status}
  Raspi             : TIDAK baca sensor apapun secara lokal lagi - cuma
                       menampung data dari kedua ESP32 lewat HTTP, lalu:
                       1. Push data TERKINI ke Supabase (live_readings) tiap LIVE_INTERVAL_SEC
                       2. Simpan LOG histori (sensor_logs) tiap LOG_INTERVAL_SEC (default 10 menit)

Jalankan: python3 main.py
Disarankan dijalankan sebagai systemd service (lihat README.md).
"""

import os

# Load file .env secara otomatis sebelum membaca konfigurasi lingkungan
from dotenv import load_dotenv
load_dotenv()

import json
import time
import threading
from datetime import datetime, timezone

from flask import Flask, request, jsonify
from supabase import create_client

# ================= KONFIGURASI =================
def env(key, default=None, cast=str):
    val = os.environ.get(key, default)
    return cast(val) if val is not None else None

SUPABASE_URL = env("SUPABASE_URL")
SUPABASE_SERVICE_KEY = env("SUPABASE_SERVICE_KEY")
DEVICE_ID = env("DEVICE_ID", "raspi-apel-01")
ESP32_API_KEY = env("ESP32_API_KEY", "ganti-dengan-kunci-rahasia-bebas")

LIVE_INTERVAL_SEC = env("LIVE_INTERVAL_SEC", "2", int)
LOG_INTERVAL_SEC = env("LOG_INTERVAL_SEC", "600", int)  # 600s = 10 menit

# Data dari ESP32 dianggap "basi" (device offline) kalau tidak ada kiriman
# baru dalam sekian detik - masing-masing indoor & outdoor independen,
# jadi kalau satu ESP32 mati, yang lain tetap tampil normal.
STALE_AFTER_SEC = 30

# ---- watchdog: supaya Raspi tidak "nyangkut offline" sampai harus di-restart manual ----
# Kalau upsert gagal terus selama RECONNECT_AFTER_SEC -> koneksi Supabase dibuat ulang.
# Kalau tetap tidak ada upsert sukses selama EXIT_AFTER_SEC (termasuk kalau request
# macet/hang) -> proses keluar, lalu systemd (Restart=always) menyalakannya lagi.
RECONNECT_AFTER_SEC = env("RECONNECT_AFTER_SEC", "120", int)
EXIT_AFTER_SEC = env("EXIT_AFTER_SEC", "600", int)
HTTP_TIMEOUT_SEC = 15

# Log 10-menit yang gagal terkirim (internet putus) disimpan di sini lalu dikirim
# ulang saat koneksi kembali, supaya grafik & laporan AC tidak bolong.
PENDING_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "pending_logs.json")
PENDING_MAX = 1000  # +-1 minggu log 10-menit

if not SUPABASE_URL or not SUPABASE_SERVICE_KEY:
    raise SystemExit("SUPABASE_URL / SUPABASE_SERVICE_KEY belum di-set (isi file .env)")


def make_client():
    try:
        from supabase.lib.client_options import ClientOptions
        return create_client(SUPABASE_URL, SUPABASE_SERVICE_KEY,
                             options=ClientOptions(postgrest_client_timeout=HTTP_TIMEOUT_SEC))
    except Exception as e:  # versi library lain: tetap jalan tanpa opsi timeout
        print("[supabase] ClientOptions tidak dipakai:", e, flush=True)
        return create_client(SUPABASE_URL, SUPABASE_SERVICE_KEY)


supabase = make_client()
client_lock = threading.Lock()
sb_health = {"last_ok": time.time(), "last_reconnect": time.time()}


def reconnect_if_needed():
    global supabase
    now = time.time()
    if now - sb_health["last_ok"] > RECONNECT_AFTER_SEC and now - sb_health["last_reconnect"] > RECONNECT_AFTER_SEC:
        print("[watchdog] upsert gagal terus, buat ulang koneksi Supabase", flush=True)
        with client_lock:
            supabase = make_client()
        sb_health["last_reconnect"] = now


def watchdog_loop():
    while True:
        time.sleep(30)
        age = time.time() - sb_health["last_ok"]
        if age > EXIT_AFTER_SEC:
            print(f"[watchdog] {int(age)} detik tanpa upsert sukses, keluar supaya di-restart systemd", flush=True)
            os._exit(1)


def load_pending():
    try:
        with open(PENDING_FILE, encoding="utf-8") as f:
            return json.load(f)
    except (FileNotFoundError, ValueError):
        return []


def save_pending(rows):
    tmp = PENDING_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(rows[-PENDING_MAX:], f)
    os.replace(tmp, PENDING_FILE)

# ================= STATE BERSAMA (thread-safe) =================
state_lock = threading.Lock()
shared_state = {
    # dari ESP32 outdoor
    "temp_out": None,
    "hum_out": None,
    "outdoor_last_seen": None,
    # dari ESP32 indoor
    "pm25": None,
    "temp_in": None,
    "hum_in": None,
    "eco2": None,        # ppm (estimasi ENS160)
    "tvoc": None,        # ppb
    "aqi": None,         # 1..5 (UBA)
    "ens_status": None,  # 0 normal, 1 warm-up, 2 initial start-up, 3 tidak valid
    "indoor_last_seen": None,
    # cache buat log 10-menit
    "last_full_reading": None,
}


# ================= FLASK: endpoint terima data dari 2 ESP32 =================
app = Flask(__name__)


def check_api_key():
    return request.headers.get("X-API-KEY") == ESP32_API_KEY


@app.route("/outdoor", methods=["POST"])
def receive_outdoor():
    if not check_api_key():
        return jsonify({"status": "unauthorized"}), 401

    data = request.get_json(force=True, silent=True) or {}
    with state_lock:
        shared_state["temp_out"] = data.get("temp_out")
        shared_state["hum_out"] = data.get("hum_out")
        shared_state["outdoor_last_seen"] = time.time()

    return jsonify({"status": "ok"})


@app.route("/indoor", methods=["POST"])
def receive_indoor():
    if not check_api_key():
        return jsonify({"status": "unauthorized"}), 401

    data = request.get_json(force=True, silent=True) or {}
    with state_lock:
        # hanya timpa field yang memang dikirim (bukan null) - biar satu
        # sensor gagal baca di ESP32 (mis. DHT11 doang) tidak ikut
        # mengosongkan sensor lain (mis. PM2.5) yang sebenarnya berhasil
        for key in ("pm25", "temp_in", "hum_in", "eco2", "tvoc", "aqi", "ens_status"):
            if data.get(key) is not None:
                shared_state[key] = data.get(key)
        shared_state["indoor_last_seen"] = time.time()

    return jsonify({"status": "ok"})


@app.route("/health", methods=["GET"])
def health():
    return jsonify({"status": "ok", "device_id": DEVICE_ID})


# ================= LOOP: push data live ke Supabase =================
def live_loop():
    while True:
        with state_lock:
            indoor_fresh = (
                shared_state["indoor_last_seen"] is not None
                and (time.time() - shared_state["indoor_last_seen"]) < STALE_AFTER_SEC
            )
            outdoor_fresh = (
                shared_state["outdoor_last_seen"] is not None
                and (time.time() - shared_state["outdoor_last_seen"]) < STALE_AFTER_SEC
            )

            pm25 = shared_state["pm25"] if indoor_fresh else None
            temp_in = shared_state["temp_in"] if indoor_fresh else None
            hum_in = shared_state["hum_in"] if indoor_fresh else None
            eco2 = shared_state["eco2"] if indoor_fresh else None
            tvoc = shared_state["tvoc"] if indoor_fresh else None
            aqi = shared_state["aqi"] if indoor_fresh else None
            ens_status = shared_state["ens_status"] if indoor_fresh else None
            temp_out = shared_state["temp_out"] if outdoor_fresh else None
            hum_out = shared_state["hum_out"] if outdoor_fresh else None

        payload = {
            "device_id": DEVICE_ID,
            "pm25": pm25,
            "temp_in": temp_in,
            "hum_in": hum_in,
            "eco2": eco2,
            "tvoc": tvoc,
            "aqi": aqi,
            "ens_status": ens_status,
            "temp_out": temp_out,
            "hum_out": hum_out,
            "updated_at": datetime.now(timezone.utc).isoformat(),
        }

        try:
            with client_lock:
                client = supabase
            client.table("live_readings").upsert(payload).execute()
            sb_health["last_ok"] = time.time()
        except Exception as e:
            print("[live_loop] gagal upsert ke Supabase:", e, flush=True)
            reconnect_if_needed()

        with state_lock:
            shared_state["last_full_reading"] = payload

        time.sleep(LIVE_INTERVAL_SEC)


# ================= LOOP: simpan log histori tiap 10 menit =================
def log_loop():
    while True:
        time.sleep(LOG_INTERVAL_SEC)
        with state_lock:
            last = shared_state.get("last_full_reading")
        if not last:
            continue

        log_payload = {
            "recorded_at": datetime.now(timezone.utc).isoformat(),
            "pm25": last["pm25"],
            "temp_in": last["temp_in"],
            "hum_in": last["hum_in"],
            "eco2": last["eco2"],
            "tvoc": last["tvoc"],
            "aqi": last["aqi"],
            "temp_out": last["temp_out"],
            "hum_out": last["hum_out"],
        }
        pending = load_pending()
        pending.append(log_payload)
        try:
            with client_lock:
                client = supabase
            client.table("sensor_logs").insert(pending).execute()
            print(f"[log_loop] {len(pending)} log tersimpan, terakhir {log_payload['recorded_at']}", flush=True)
            pending = []
        except Exception as e:
            print(f"[log_loop] gagal insert log ({len(pending)} antre, dikirim ulang nanti):", e, flush=True)
        save_pending(pending)


if __name__ == "__main__":
    threading.Thread(target=live_loop, daemon=True).start()
    threading.Thread(target=log_loop, daemon=True).start()
    threading.Thread(target=watchdog_loop, daemon=True).start()
    # host 0.0.0.0 supaya kedua ESP32 di jaringan yang sama bisa akses
    app.run(host="0.0.0.0", port=5000)
SMARTLAB_MAIN_EOF
echo "== main.py diperbarui"

# 3) watchdog jaringan
cat > net_watchdog.sh <<SMARTLAB_NET_EOF
#!/bin/bash
# Cek koneksi ke Supabase tiap 5 menit. Gagal 15 menit -> nyalakan ulang jaringan, 30 menit -> reboot.
STATE=/var/lib/smartlab-netwatch.count
URL=\$(grep -E '^SUPABASE_URL=' "$DIR/.env" | cut -d= -f2- | tr -d '"\r')
command -v iw >/dev/null && iw dev wlan0 set power_save off 2>/dev/null
if curl -s -o /dev/null -m 15 "\$URL/rest/v1/"; then echo 0 > \$STATE; exit 0; fi
n=\$(( \$(cat \$STATE 2>/dev/null || echo 0) + 1 )); echo \$n > \$STATE
logger -t smartlab-netwatch "Supabase tidak terjangkau (gagal ke-\$n)"
if [ \$n -ge 6 ]; then
  echo 0 > \$STATE; logger -t smartlab-netwatch "30 menit tanpa koneksi, reboot"; /sbin/reboot
elif [ \$n -eq 3 ]; then
  logger -t smartlab-netwatch "15 menit tanpa koneksi, nyalakan ulang jaringan"
  if command -v nmcli >/dev/null; then nmcli networking off; sleep 5; nmcli networking on
  else ip link set wlan0 down; sleep 5; ip link set wlan0 up; fi
fi
SMARTLAB_NET_EOF
chmod +x net_watchdog.sh

# 4) service utama
[ -f /etc/systemd/system/smartlab.service ] && sudo cp /etc/systemd/system/smartlab.service "$DIR/smartlab.service.bak"
sudo tee /etc/systemd/system/smartlab.service >/dev/null <<SMARTLAB_SVC_EOF
[Unit]
Description=SmartLab APEL - sensor collector
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
Type=simple
User=$USR
WorkingDirectory=$DIR
EnvironmentFile=$DIR/.env
Environment=PYTHONUNBUFFERED=1
ExecStart=$PY $DIR/main.py
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
SMARTLAB_SVC_EOF

# 5) service + timer watchdog jaringan
sudo tee /etc/systemd/system/smartlab-netwatch.service >/dev/null <<SMARTLAB_NWS_EOF
[Unit]
Description=SmartLab - cek koneksi jaringan Raspi

[Service]
Type=oneshot
ExecStart=/bin/bash $DIR/net_watchdog.sh
SMARTLAB_NWS_EOF
sudo tee /etc/systemd/system/smartlab-netwatch.timer >/dev/null <<'SMARTLAB_NWT_EOF'
[Unit]
Description=SmartLab - jalankan cek koneksi tiap 5 menit

[Timer]
OnBootSec=5min
OnUnitActiveSec=5min

[Install]
WantedBy=timers.target
SMARTLAB_NWT_EOF

# 6) matikan power saving WiFi
if [ -d /etc/NetworkManager/conf.d ]; then
  printf '[connection]\nwifi.powersave = 2\n' | sudo tee /etc/NetworkManager/conf.d/wifi-powersave-off.conf >/dev/null
fi
command -v iw >/dev/null && sudo iw dev wlan0 set power_save off 2>/dev/null || true

# 7) aktifkan semuanya
sudo systemctl daemon-reload
sudo systemctl enable smartlab.service >/dev/null
sudo systemctl enable --now smartlab-netwatch.timer
sudo systemctl restart smartlab.service
sleep 4
echo
echo "== Status:"
systemctl is-active smartlab.service && echo "   smartlab: jalan"
systemctl is-active smartlab-netwatch.timer && echo "   watchdog jaringan: aktif"
echo
echo "== 15 baris log terakhir (cek tidak ada error):"
journalctl -u smartlab -n 15 --no-pager
