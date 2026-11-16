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

if not SUPABASE_URL or not SUPABASE_SERVICE_KEY:
    raise SystemExit("SUPABASE_URL / SUPABASE_SERVICE_KEY belum di-set (isi file .env)")

supabase = create_client(SUPABASE_URL, SUPABASE_SERVICE_KEY)

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
            supabase.table("live_readings").upsert(payload).execute()
        except Exception as e:
            print("[live_loop] gagal upsert ke Supabase:", e)

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
        try:
            supabase.table("sensor_logs").insert(log_payload).execute()
            print("[log_loop] log tersimpan:", log_payload["recorded_at"])
        except Exception as e:
            print("[log_loop] gagal insert log:", e)


if __name__ == "__main__":
    threading.Thread(target=live_loop, daemon=True).start()
    threading.Thread(target=log_loop, daemon=True).start()
    # host 0.0.0.0 supaya kedua ESP32 di jaringan yang sama bisa akses
    app.run(host="0.0.0.0", port=5000)
