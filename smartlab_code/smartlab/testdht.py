"""
Tes cepat DHT11 indoor (GPIO4 / pin fisik 7), TERPISAH dari Flask & Supabase.
Jalankan: python3 test_dht11.py
Ctrl+C untuk berhenti.

Kalau baris "Gagal baca" muncul SESEKALI itu normal (DHT11 memang suka gagal
timing-nya). Tapi kalau GAGAL TERUS tanpa henti, biasanya karena:
- salah pin data (pastikan memang GPIO4 / pin fisik 7, bukan nomor pin lain)
- tidak ada resistor pull-up ~10k ohm dari DATA ke VCC (modul breakout biasanya
  sudah punya ini bawaan, tapi kalau sensor telanjang/bare module, wajib ditambah)
- power ke VCC tidak stabil (coba dari 5V, bukan 3.3V, kalau modulnya support)
- kabel data kepanjangan / longgar
"""
import time
import board
import adafruit_dht

dht = adafruit_dht.DHT11(board.D4)

print("Mulai baca DHT11 di GPIO4 (pin fisik 7)... Ctrl+C untuk stop\n")
while True:
    try:
        t = dht.temperature
        h = dht.humidity
        print(f"Suhu: {t} C   Kelembapan: {h} %")
    except RuntimeError as e:
        print("Gagal baca (biasa terjadi sesekali):", e)
    except Exception as e:
        print("ERROR TAK TERDUGA:", repr(e))
    time.sleep(2)
