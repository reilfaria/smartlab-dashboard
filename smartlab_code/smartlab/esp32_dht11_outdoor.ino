/*
  SmartLab Lab APEL UNTIRTA - ESP32 DHT11 Outdoor
  =================================================
  Baca DHT11 (luar ruangan) lalu kirim ke Raspberry Pi
  lewat HTTP POST di jaringan WiFi lokal yang sama.

  Wiring DHT11 -> ESP32:
    VCC  -> 3V3
    GND  -> GND
    DATA -> GPIO4 (bebas ganti sesuai kabel kamu, sesuaikan DHTPIN di bawah)

  Library yang dibutuhkan (install via Library Manager):
    - "DHT sensor library" by Adafruit
    - "Adafruit Unified Sensor"
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>

#define DHTPIN  4
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ---- Ganti sesuai jaringan & Raspi kamu ----
const char* WIFI_SSID     = "NAMA_WIFI";
const char* WIFI_PASSWORD = "PASSWORD_WIFI";
const char* RASPI_URL     = "http://IP_RASPI_DISINI:5000/outdoor"; // contoh: http://192.168.1.20:5000/outdoor
const char* API_KEY       = "ganti-dengan-kunci-rahasia-bebas";    // harus SAMA dengan ESP32_API_KEY di .env Raspi

const unsigned long SEND_INTERVAL_MS = 5000; // kirim tiap 5 detik

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menyambung ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Tersambung, IP ESP32: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  float hum = dht.readHumidity();
  float temp = dht.readTemperature();

  if (isnan(hum) || isnan(temp)) {
    Serial.println("Gagal baca DHT11, coba lagi siklus berikutnya");
  } else {
    HTTPClient http;
    http.begin(RASPI_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-KEY", API_KEY);

    String payload = "{\"temp_out\":" + String(temp, 1) +
                      ",\"hum_out\":" + String(hum, 1) + "}";

    int httpCode = http.POST(payload);
    Serial.printf("Kirim data -> suhu=%.1fC lembap=%.1f%%  status HTTP=%d\n", temp, hum, httpCode);
    http.end();
  }

  delay(SEND_INTERVAL_MS);
}
