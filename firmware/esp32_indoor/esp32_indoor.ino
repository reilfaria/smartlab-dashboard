/*
  SmartLab Lab APEL UNTIRTA - ESP32 INDOOR (PM2.5 + ENS160/AHT21)
  Wiring ZH03B       : VCC->5V, GND->GND, TXD sensor->GPIO16, RXD sensor->GPIO17
  Wiring ENS160+AHT21: VIN->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22 (CS, ADD, INT kosong)
  Library            : tidak perlu library tambahan (Wire bawaan Arduino-ESP32)

  Data yang dikirim ke Raspi (POST /indoor):
    temp_in, hum_in  -> dari AHT21
    eco2 (ppm), tvoc (ppb), aqi (1..5) -> dari ENS160
    ens_status       -> 0 normal, 1 warm-up, 2 initial start-up, 3 tidak valid
    pm25             -> dari ZH03B

  Catatan:
  - eCO2 ENS160 adalah ESTIMASI CO2 dari kadar VOC, bukan pengukuran CO2 langsung (NDIR).
  - Pemakaian pertama: biarkan modul menyala +-48 jam (burn-in), dan +-1 jam pertama
    statusnya "initial start-up". Tiap dinyalakan ulang ada warm-up +-3 menit.
  - Heater ENS160 menghangatkan board, jadi AHT21 cenderung membaca suhu lebih tinggi.
    Bandingkan dengan termometer acuan, lalu isi TEMP_OFFSET_C / HUM_OFFSET_PCT.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <HardwareSerial.h>

// ---------- pin ----------
#define SDA_PIN 21
#define SCL_PIN 22
#define PM_RX_PIN 16
#define PM_TX_PIN 17
HardwareSerial pmSerial(2);

// ---------- kalibrasi AHT21 (koreksi efek panas ENS160) ----------
const float TEMP_OFFSET_C  = 0.0;   // contoh: -2.0 kalau AHT21 terbaca 2 C lebih tinggi
const float HUM_OFFSET_PCT = 0.0;

// ---------- jaringan ----------
const char* WIFI_SSID      = "UNTIRTA";
const char* WIFI_PASSWORD  = "untirtajawara";
const char* RASPI_HOST     = "labapel";                          // hostname Raspi -> labapel.local
const char* RASPI_FALLBACK = "http://10.17.149.177:5000/indoor"; // cadangan kalau pencarian nama gagal
const char* API_KEY        = "labapel123";

const unsigned long SEND_INTERVAL_MS = 2000;

String raspiUrl = RASPI_FALLBACK;

// ================= I2C: AHT21 + ENS160 =================
const uint8_t AHT_ADDR = 0x38;
uint8_t ensAddr = 0;          // 0x53 atau 0x52, diisi saat deteksi
bool ahtOK = false;
bool ensOK = false;

const uint8_t REG_PART_ID = 0x00;
const uint8_t REG_OPMODE  = 0x10;
const uint8_t REG_TEMP_IN = 0x13;
const uint8_t REG_RH_IN   = 0x15;
const uint8_t REG_STATUS  = 0x20;  // lalu berurutan: AQI, TVOC(2 byte), eCO2(2 byte)

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
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ensAddr, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool ahtInit() {
  if (!i2cPresent(AHT_ADDR)) {
    Serial.println("AHT21 : tidak terdeteksi di 0x38");
    return false;
  }
  delay(40);
  if (Wire.requestFrom((int)AHT_ADDR, 1) != 1) return false;
  uint8_t status = Wire.read();
  if (!(status & 0x08)) {          // belum terkalibrasi -> kirim perintah init
    Wire.beginTransmission(AHT_ADDR);
    Wire.write(0xBE); Wire.write(0x08); Wire.write(0x00);
    Wire.endTransmission();
    delay(20);
  }
  Serial.println("AHT21 : OK (0x38)");
  return true;
}

bool ahtRead(float& tempC, float& rh) {
  Wire.beginTransmission(AHT_ADDR);
  Wire.write(0xAC); Wire.write(0x33); Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  delay(80);

  uint8_t b[7];
  uint8_t tries = 0;
  do {
    if (Wire.requestFrom((int)AHT_ADDR, 7) != 7) return false;
    for (uint8_t i = 0; i < 7; i++) b[i] = Wire.read();
    if (!(b[0] & 0x80)) break;     // bit 7 = busy
    delay(10);
  } while (++tries < 10);
  if (b[0] & 0x80) return false;

  uint32_t rawH = ((uint32_t)b[1] << 12) | ((uint32_t)b[2] << 4) | (b[3] >> 4);
  uint32_t rawT = ((uint32_t)(b[3] & 0x0F) << 16) | ((uint32_t)b[4] << 8) | b[5];
  rh    = rawH * 100.0f / 1048576.0f;
  tempC = rawT * 200.0f / 1048576.0f - 50.0f;
  return true;
}

void ensSetMode(uint8_t mode) { ensWrite(REG_OPMODE, &mode, 1); }

void ensArm(bool withReset) {
  if (withReset) { ensSetMode(0xF0); delay(200); }   // reset chip
  ensSetMode(0x01); delay(20);                        // idle
  ensSetMode(0x02); delay(50);                        // standard gas sensing
}

bool ensInit() {
  if (i2cPresent(0x53)) ensAddr = 0x53;
  else if (i2cPresent(0x52)) ensAddr = 0x52;
  else {
    Serial.println("ENS160: tidak terdeteksi di 0x52 / 0x53");
    return false;
  }
  uint8_t id[2];
  if (!ensRead(REG_PART_ID, id, 2)) return false;
  uint16_t part = id[0] | (id[1] << 8);
  if (part != 0x0160 && part != 0x0161) {
    Serial.printf("ENS160: PART_ID 0x%04X tidak cocok\n", (unsigned)part);
    return false;
  }
  ensArm(false);
  Serial.printf("ENS160: OK (0x%02X)\n", ensAddr);
  return true;
}

void ensSetCompensation(float tempC, float rh) {
  uint16_t t = (uint16_t)((tempC + 273.15f) * 64.0f);
  uint16_t h = (uint16_t)(rh * 512.0f);
  uint8_t tb[2] = { (uint8_t)(t & 0xFF), (uint8_t)(t >> 8) };
  uint8_t hb[2] = { (uint8_t)(h & 0xFF), (uint8_t)(h >> 8) };
  ensWrite(REG_TEMP_IN, tb, 2);
  ensWrite(REG_RH_IN, hb, 2);
}

// hasil true kalau data ENS160 layak dikirim (AQI 1..5 dan status bukan "tidak valid")
bool ensReadAll(uint16_t& eco2, uint16_t& tvoc, uint8_t& aqi, uint8_t& validity) {
  static uint8_t deadCount = 0, rearmCount = 0;
  uint8_t d[6];
  if (!ensRead(REG_STATUS, d, 6)) return false;
  validity = (d[0] >> 2) & 0x03;
  aqi      = d[1] & 0x07;
  tvoc     = d[2] | (d[3] << 8);
  eco2     = d[4] | (d[5] << 8);

  if (aqi < 1 || aqi > 5) {        // AQI 0 = ENS160 belum/tidak sedang mengukur
    if (++deadCount >= 3) {
      deadCount = 0;
      if (rearmCount < 5) {
        Serial.println("ENS160 belum mengukur, aktifkan ulang mode standard...");
        ensArm(rearmCount >= 1);
        rearmCount++;
      }
    }
    return false;
  }
  deadCount = 0;
  rearmCount = 0;
  return validity != 3;
}

// ================= WiFi / Raspi =================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menyambung ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Tersambung, IP ESP32 indoor: ");
  Serial.println(WiFi.localIP());
}

bool resolveRaspi() {
  IPAddress ip = MDNS.queryHost(RASPI_HOST, 3000);
  if ((uint32_t)ip == 0) {
    Serial.println("Nama Raspi belum ketemu lewat mDNS, pakai alamat terakhir: " + raspiUrl);
    return false;
  }
  raspiUrl = "http://" + ip.toString() + ":5000/indoor";
  Serial.println("Raspi ketemu di: " + raspiUrl);
  return true;
}

// ================= ZH03B PM2.5 =================
bool readPM25(int &pm25Out, unsigned long timeoutMs = 1500) {
  unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    if (pmSerial.available() < 1) continue;
    if (pmSerial.read() != 0x42) continue;

    unsigned long b1Deadline = millis() + 50;
    while (pmSerial.available() < 1 && millis() < b1Deadline) { }
    if (pmSerial.available() < 1 || pmSerial.read() != 0x4D) continue;

    uint8_t rest[30];
    int got = 0;
    unsigned long restDeadline = millis() + 200;
    while (got < 30 && millis() < restDeadline) {
      if (pmSerial.available()) rest[got++] = pmSerial.read();
    }
    if (got < 30) continue;

    uint16_t checksum = 0x42 + 0x4D;
    for (int i = 0; i < 28; i++) checksum += rest[i];
    uint16_t recvChecksum = (rest[28] << 8) | rest[29];
    if (checksum != recvChecksum) continue;

    pm25Out = (rest[4] << 8) | rest[5];
    return true;
  }
  return false;
}

// ================= Arduino =================
void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  ahtOK = ahtInit();
  ensOK = ensInit();
  pmSerial.begin(9600, SERIAL_8N1, PM_RX_PIN, PM_TX_PIN);
  connectWiFi();
  MDNS.begin("esp32-indoor");
  resolveRaspi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    resolveRaspi();
  }

  // coba init ulang kalau modul sempat lepas / belum terdeteksi saat boot
  if (!ahtOK) ahtOK = ahtInit();
  if (!ensOK) ensOK = ensInit();

  float temp = NAN, hum = NAN;
  bool ahtReadOk = ahtOK && ahtRead(temp, hum);
  if (ahtReadOk) {
    if (ensOK) ensSetCompensation(temp, hum);   // kompensasi pakai nilai mentah
    temp += TEMP_OFFSET_C;
    hum  = constrain(hum + HUM_OFFSET_PCT, 0.0f, 100.0f);
  }

  uint16_t eco2 = 0, tvoc = 0;
  uint8_t aqi = 0, validity = 3;
  bool ensReadOk = ensOK && ensReadAll(eco2, tvoc, aqi, validity);

  int pm25 = -1;
  bool pmOk = readPM25(pm25);

  if (ahtReadOk || ensReadOk || pmOk) {
    HTTPClient http;
    http.begin(raspiUrl);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-KEY", API_KEY);

    String payload = "{";
    payload += "\"temp_in\":"    + (ahtReadOk ? String(temp, 1) : String("null")) + ",";
    payload += "\"hum_in\":"     + (ahtReadOk ? String(hum, 1)  : String("null")) + ",";
    payload += "\"eco2\":"       + (ensReadOk ? String(eco2)    : String("null")) + ",";
    payload += "\"tvoc\":"       + (ensReadOk ? String(tvoc)    : String("null")) + ",";
    payload += "\"aqi\":"        + (ensReadOk ? String(aqi)     : String("null")) + ",";
    payload += "\"ens_status\":" + (ensReadOk ? String(validity): String("null")) + ",";
    payload += "\"pm25\":"       + (pmOk      ? String(pm25)    : String("null"));
    payload += "}";

    int httpCode = http.POST(payload);
    Serial.printf("Kirim -> suhu=%.1fC lembap=%.1f%% eCO2=%u TVOC=%u AQI=%u st=%u pm25=%d  HTTP=%d\n",
                  temp, hum, eco2, tvoc, aqi, validity, pm25, httpCode);
    http.end();

    if (httpCode <= 0) resolveRaspi();
  } else {
    Serial.println("AHT21, ENS160, dan PM2.5 semua gagal baca, skip kirim siklus ini");
  }

  delay(SEND_INTERVAL_MS);
}
