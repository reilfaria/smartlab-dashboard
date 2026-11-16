/*
  SmartLab Lab APEL UNTIRTA - NodeMCU v3 (ESP8266) OUTDOOR (DHT11)
  Wiring DHT11 : VCC->3V3, GND->G, DATA->D2 (GPIO4)
  Board        : "NodeMCU 1.0 (ESP-12E Module)"
  Library      : "DHT sensor library" (Adafruit), "Adafruit Unified Sensor"
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>
#include <DHT.h>

#define DHTPIN   D2
#define DHTTYPE  DHT11
DHT dht(DHTPIN, DHTTYPE);

const char* WIFI_SSID      = "UNTIRTA";
const char* WIFI_PASSWORD  = "untirtajawara";
const char* RASPI_HOST     = "labapel.local";                     // hostname Raspi + .local
const char* RASPI_FALLBACK = "http://10.17.149.177:5000/outdoor"; // cadangan kalau pencarian nama gagal
const char* API_KEY        = "labapel123";

const unsigned long SEND_INTERVAL_MS = 2000;

WiFiClient wifiClient;
String raspiUrl = RASPI_FALLBACK;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menyambung ke WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Tersambung, IP NodeMCU outdoor: ");
  Serial.println(WiFi.localIP());
}

bool resolveRaspi() {
  IPAddress ip;
  if (WiFi.hostByName(RASPI_HOST, ip, 3000) && ip != IPAddress(0, 0, 0, 0)) {
    raspiUrl = "http://" + ip.toString() + ":5000/outdoor";
    Serial.println("Raspi ketemu di: " + raspiUrl);
    return true;
  }
  Serial.println("Nama Raspi belum ketemu, pakai alamat terakhir: " + raspiUrl);
  return false;
}

void setup() {
  Serial.begin(115200);
  dht.begin();
  connectWiFi();
  MDNS.begin("nodemcu-outdoor");
  resolveRaspi();
}

void loop() {
  MDNS.update();

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    resolveRaspi();
  }

  float hum  = dht.readHumidity();
  float temp = dht.readTemperature();

  if (!isnan(hum) && !isnan(temp)) {
    HTTPClient http;
    http.begin(wifiClient, raspiUrl);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-KEY", API_KEY);

    String payload = "{";
    payload += "\"temp_out\":" + String(temp, 1) + ",";
    payload += "\"hum_out\":"  + String(hum, 1);
    payload += "}";

    int httpCode = http.POST(payload);
    Serial.printf("Kirim -> suhu=%.1fC lembap=%.1f%%  status HTTP=%d\n", temp, hum, httpCode);
    http.end();

    if (httpCode <= 0) resolveRaspi();
  } else {
    Serial.println("DHT11 gagal baca, skip kirim siklus ini");
  }

  delay(SEND_INTERVAL_MS);
}