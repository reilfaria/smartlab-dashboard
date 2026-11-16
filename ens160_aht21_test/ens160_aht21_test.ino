/*
  Tes modul ENS160 + AHT21 di ESP32 (I2C)  -- v2
  - Tanpa library tambahan, cukup Wire bawaan Arduino-ESP32
  - Urutan: scan I2C -> init AHT21 -> init ENS160 -> baca tiap 2 detik
  - Serial Monitor: 115200 baud

  Perubahan v2:
  - Mode ENS160 diverifikasi (OPMODE dibaca ulang), bukan sekadar dikirim
  - Kalau ENS160 berhenti mengukur (AQI 0 terus), mode diaktifkan ulang otomatis
  - Tiap baris menampilkan OPMODE dan STATUS mentah untuk diagnosa

  Kabel (ESP32 DevKit):
    VIN -> 3V3 | GND -> GND | SDA -> GPIO21 | SCL -> GPIO22
    3V3, CS, ADD, INT di modul dibiarkan kosong
  Kalau pakai pin lain (atau ESP32-S3/C3), ubah SDA_PIN dan SCL_PIN di bawah.

  Catatan: pesan "NACK" / "i2c error" di Serial saat scan itu normal
  (alamat kosong memang tidak menjawab).
*/

#include <Wire.h>

#define SDA_PIN 21
#define SCL_PIN 22

const uint8_t AHT_ADDR = 0x38;  // alamat AHT21 (tetap)
uint8_t ensAddr = 0;            // 0x53 atau 0x52, diisi saat scan
bool ahtOK = false;
bool ensOK = false;

// Register ENS160
const uint8_t REG_PART_ID = 0x00;
const uint8_t REG_OPMODE  = 0x10;
const uint8_t REG_TEMP_IN = 0x13;
const uint8_t REG_RH_IN   = 0x15;
const uint8_t REG_STATUS  = 0x20;  // lalu berurutan: AQI, TVOC(2 byte), eCO2(2 byte)

const char* VALIDITY_TXT[4] = {
  "normal",
  "warm-up (+-3 menit pertama)",
  "initial start-up (+-1 jam pemakaian pertama)",
  "tidak valid"
};

// ---------- helper I2C ----------
bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

bool ensWrite(uint8_t reg, const uint8_t* data, uint8_t len) {
  Wire.beginTransmission(ensAddr);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(data[i]);
  return Wire.endTransmission() == 0;
}

bool ensRead(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(ensAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start
  if (Wire.requestFrom((int)ensAddr, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

void scanI2C() {
  Serial.println("Scan I2C...");
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cPresent(a)) {
      Serial.printf("  ditemukan: 0x%02X\n", a);
      found++;
    }
  }
  if (found == 0) Serial.println("  TIDAK ADA perangkat -> cek kabel VIN/GND/SDA/SCL.");
  if (i2cPresent(0x53)) ensAddr = 0x53;
  else if (i2cPresent(0x52)) ensAddr = 0x52;
}

// ---------- AHT21 ----------
bool ahtInit() {
  if (!i2cPresent(AHT_ADDR)) {
    Serial.println("AHT21 : tidak terdeteksi di 0x38");
    return false;
  }
  delay(40);
  if (Wire.requestFrom((int)AHT_ADDR, 1) != 1) return false;
  uint8_t status = Wire.read();
  if (!(status & 0x08)) {  // bit kalibrasi belum set -> kirim perintah init
    Wire.beginTransmission(AHT_ADDR);
    Wire.write(0xBE);
    Wire.write(0x08);
    Wire.write(0x00);
    Wire.endTransmission();
    delay(20);
  }
  Serial.println("AHT21 : OK (0x38)");
  return true;
}

bool ahtRead(float& tempC, float& rh) {
  Wire.beginTransmission(AHT_ADDR);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(80);  // waktu konversi

  uint8_t b[7];
  uint8_t tries = 0;
  do {
    if (Wire.requestFrom((int)AHT_ADDR, 7) != 7) return false;
    for (uint8_t i = 0; i < 7; i++) b[i] = Wire.read();
    if (!(b[0] & 0x80)) break;  // bit 7 = busy
    delay(10);
  } while (++tries < 10);
  if (b[0] & 0x80) return false;

  uint32_t rawH = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
  uint32_t rawT = ((uint32_t)(b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5];
  rh    = rawH * 100.0f / 1048576.0f;
  tempC = rawT * 200.0f / 1048576.0f - 50.0f;
  return true;
}

// ---------- ENS160 ----------
bool ensSetMode(uint8_t mode) {
  return ensWrite(REG_OPMODE, &mode, 1);
}

uint8_t ensGetMode() {
  uint8_t m = 0xFF;  // 0xFF = gagal baca
  ensRead(REG_OPMODE, &m, 1);
  return m;
}

// Aktifkan mode standard (0x02). withReset = true -> reset chip dulu (0xF0).
void ensArm(bool withReset) {
  if (withReset) {
    ensSetMode(0xF0);  // reset chip
    delay(200);
  }
  ensSetMode(0x01); delay(20);  // idle
  ensSetMode(0x02); delay(50);  // standard gas sensing
  Serial.printf("ENS160: set mode standard%s -> OPMODE terbaca 0x%02X (harusnya 0x02)\n",
                withReset ? " (setelah reset)" : "", (unsigned)ensGetMode());
}

bool ensInit() {
  if (ensAddr == 0) {
    Serial.println("ENS160: tidak terdeteksi di 0x52 / 0x53");
    return false;
  }
  uint8_t id[2];
  if (!ensRead(REG_PART_ID, id, 2)) {
    Serial.println("ENS160: gagal baca PART_ID");
    return false;
  }
  uint16_t part = id[0] | (id[1] << 8);
  Serial.printf("ENS160: alamat 0x%02X, PART_ID 0x%04X\n", ensAddr, (unsigned)part);
  if (part != 0x0160 && part != 0x0161) {
    Serial.println("  PART_ID tidak cocok (harusnya 0x0160) -> cek modul/kabel");
    return false;
  }
  ensArm(false);
  return true;
}

// kirim suhu & kelembapan dari AHT21 supaya hitungan ENS160 lebih akurat
void ensSetCompensation(float tempC, float rh) {
  uint16_t t = (uint16_t)((tempC + 273.15f) * 64.0f);
  uint16_t h = (uint16_t)(rh * 512.0f);
  uint8_t tb[2] = { (uint8_t)(t & 0xFF), (uint8_t)(t >> 8) };
  uint8_t hb[2] = { (uint8_t)(h & 0xFF), (uint8_t)(h >> 8) };
  ensWrite(REG_TEMP_IN, tb, 2);
  ensWrite(REG_RH_IN, hb, 2);
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== Tes ENS160 + AHT21 (v2) ===");

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  scanI2C();
  ahtOK = ahtInit();
  ensOK = ensInit();

  if (!ahtOK && !ensOK) {
    Serial.println("Dua-duanya gagal. Cek: VIN ke 3V3, GND, SDA/SCL tidak tertukar, solderan pin.");
  }
  Serial.println();
}

void loop() {
  static uint8_t deadCount = 0;   // berapa kali berturut-turut AQI di luar 1..5
  static uint8_t rearmCount = 0;  // berapa kali sudah diaktifkan ulang

  float t = 0, h = 0;
  bool gotAht = ahtOK && ahtRead(t, h);

  if (gotAht) Serial.printf("AHT21  | suhu %.2f C | RH %.1f %%\n", t, h);
  else if (ahtOK) Serial.println("AHT21  | gagal baca");

  if (ensOK) {
    if (gotAht) ensSetCompensation(t, h);
    uint8_t d[6];
    if (ensRead(REG_STATUS, d, 6)) {
      uint8_t status   = d[0];
      uint8_t validity = (status >> 2) & 0x03;
      uint8_t aqi      = d[1] & 0x07;
      uint16_t tvoc    = d[2] | (d[3] << 8);
      uint16_t eco2    = d[4] | (d[5] << 8);
      uint8_t mode     = ensGetMode();

      Serial.printf("ENS160 | AQI %u (1=bagus..5=buruk) | TVOC %u ppb | eCO2 %u ppm\n",
                    (unsigned)aqi, (unsigned)tvoc, (unsigned)eco2);
      Serial.printf("         status: %s | OPMODE 0x%02X | STATUS 0x%02X%s\n",
                    VALIDITY_TXT[validity], (unsigned)mode, (unsigned)status,
                    (status & 0x40) ? " | ERROR!" : "");

      // AQI valid hanya 1..5. AQI 0 berarti ENS160 belum/tidak sedang mengukur.
      if (aqi < 1 || aqi > 5) {
        if (++deadCount >= 3) {
          deadCount = 0;
          if (rearmCount < 5) {
            Serial.println("  -> ENS160 belum mengukur (AQI 0), aktifkan ulang mode standard...");
            ensArm(rearmCount >= 1);  // percobaan pertama tanpa reset, berikutnya dengan reset
            rearmCount++;
          } else if (rearmCount == 5) {
            Serial.println("  -> Masih belum mengukur. Coba cabut-colok power modul (VIN) lalu reset ESP32.");
            rearmCount++;
          }
        }
      } else {
        deadCount = 0;
        rearmCount = 0;
      }
    } else {
      Serial.println("ENS160 | gagal baca");
    }
  }

  Serial.println();
  delay(2000);
}