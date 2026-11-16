import serial

PORT = "/dev/serial0"
BAUD = 9600

ser = serial.Serial(PORT, BAUD, timeout=2)
print(f"Membuka {PORT} @ {BAUD} baud... Ctrl+C untuk stop\n")

while True:
    b0 = ser.read(1)
    if not b0:
        print("Tidak ada byte masuk sama sekali dalam 2 detik")
        continue
    if b0 != b"\x42":
        continue
    b1 = ser.read(1)
    if b1 != b"\x4d":
        continue
    rest = ser.read(30)
    if len(rest) < 30:
        continue

    frame = b0 + b1 + rest
    checksum = sum(frame[:30]) & 0xFFFF
    recv_checksum = (frame[30] << 8) | frame[31]
    pm1_0 = (frame[4] << 8) | frame[5]
    pm2_5 = (frame[6] << 8) | frame[7]
    pm10 = (frame[8] << 8) | frame[9]

    status = "OK" if checksum == recv_checksum else "CHECKSUM SALAH"
    print(f"Frame [{status}]  PM1.0={pm1_0}  PM2.5={pm2_5}  PM10={pm10}")
