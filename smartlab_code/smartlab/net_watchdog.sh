#!/bin/bash
# SmartLab - watchdog jaringan Raspi (dijalankan tiap 5 menit oleh smartlab-netwatch.timer)
# Cek koneksi ke Supabase. Gagal 3x berturut-turut (+-15 menit) -> nyalakan ulang jaringan.
# Gagal 6x berturut-turut (+-30 menit) -> reboot Raspi (sama seperti restart manual).
ENV_FILE=/home/pi/smartlab/raspberry_pi/.env
STATE=/var/lib/smartlab-netwatch.count
URL=$(grep -E '^SUPABASE_URL=' "$ENV_FILE" | cut -d= -f2- | tr -d '"\r')

# respons HTTP apa pun (termasuk 401) berarti internet & Supabase terjangkau
if curl -s -o /dev/null -m 15 "$URL/rest/v1/"; then
  echo 0 > "$STATE"
  exit 0
fi

n=$(( $(cat "$STATE" 2>/dev/null || echo 0) + 1 ))
echo "$n" > "$STATE"
logger -t smartlab-netwatch "Supabase tidak terjangkau (gagal ke-$n)"

if [ "$n" -ge 6 ]; then
  echo 0 > "$STATE"
  logger -t smartlab-netwatch "30 menit tanpa koneksi, reboot"
  /sbin/reboot
elif [ "$n" -eq 3 ]; then
  logger -t smartlab-netwatch "15 menit tanpa koneksi, nyalakan ulang jaringan"
  if command -v nmcli >/dev/null; then
    nmcli networking off; sleep 5; nmcli networking on
  else
    ip link set wlan0 down; sleep 5; ip link set wlan0 up
  fi
fi
