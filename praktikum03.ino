#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// Kredensial Jaringan Wi-Fi
const char* ssid = "Vivo Y21A";
const char* password = "lilis123";

// Konfigurasi IP Statis
IPAddress local_IP(172, 20, 10, 3);
IPAddress gateway(172, 20, 10, 1);
IPAddress subnet(255, 255, 255, 0);

// Konfigurasi Broker MQTT Publik
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* clientId = "ESP8266_LabIoT_25782011_Task67";

// Definisi Topik Tugas 6.7
const char* topicPublish   = "kampus/labiot/25782011/telemetri";
const char* topicSubscribe = "kampus/labiot/25782011/kendali";
const char* topicAlert     = "kampus/labiot/25782011/alert";
const char* topicStatus    = "kampus/labiot/25782011/status";

// Alokasi Pin Perangkat Keras
const byte dhtPin = 2; // Pin D4 (GPIO 2)
const byte ledPin = 5; // Pin D1 (GPIO 5)
#define DHTTYPE DHT11

DHT dht(dhtPin, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

unsigned long lastTelemetryTime = 0;
const unsigned long telemetryInterval = 4000; // Siklus kirim 4 detik

bool alertSent = false; // Flag/latch untuk mencegah spamming alert ke broker

void callback(char* topic, byte* payload, unsigned int length) {
  char incomingBuffer[length + 1];
  memcpy(incomingBuffer, payload, length);
  incomingBuffer[length] = '\0';

  Serial.println("----------------------------------------");
  Serial.printf("[KONTROL DITERIMA] Topik: %s | Payload: %s\n", topic, incomingBuffer);

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, incomingBuffer);
  if (err) {
    Serial.println("Gagal membedah payload JSON!");
    return;
  }

  // Kendali Manual Override saat kondisi normal
  if (doc.containsKey("led")) {
    int val = doc["led"];
    digitalWrite(ledPin, val == 1 ? HIGH : LOW);
    Serial.printf("--> [INDIKATOR] Status LED diubah manual: %s\n", val == 1 ? "MENYALA" : "PADAM");
  }
  Serial.println("----------------------------------------");
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Menghubungkan kembali ke Broker...");
    
    // Konfigurasi Last Will and Testament (LWT)
    // Parameter LWT: client.connect(clientId, user, pass, lwtTopic, lwtQoS, lwtRetain, lwtMessage)
    bool connected = client.connect(
      clientId, 
      NULL, NULL, 
      topicStatus, 1, true, 
      "{\"status\":\"OFFLINE\",\"keterangan\":\"Perangkat Terputus Tak Terduga\"}"
    );

    if (connected) {
      Serial.println(" Sukses Terhubung!");
      
      // Daftarkan subskripsi topik kendali
      client.subscribe(topicSubscribe);
      Serial.printf("Subskripsi Topik: %s\n", topicSubscribe);

      // Publikasikan status ONLINE sesaat setelah terhubung
      const char* onlineMsg = "{\"status\":\"ONLINE\",\"keterangan\":\"Perangkat Beroperasi Normal\"}";
      client.publish(topicStatus, onlineMsg, true);
      Serial.println("[STATUS] Publikasi pesan ONLINE berhasil.");
    } else {
      Serial.printf("Gagal (rc=%d). Coba lagi dalam 4 detik...\n", client.state());
      delay(4000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  dht.begin();

  WiFi.mode(WIFI_STA);
  
  // Menerapkan Konfigurasi IP Statis
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("Gagal mengonfigurasi IP statis!");
  }

  WiFi.begin(ssid, password);
  Serial.print("Menghubungkan Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Terkoneksi!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  client.setServer(mqttServer, mqttPort);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // Memelihara sesi TCP dan memproses callback masuk

  unsigned long now = millis();
  if (now - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = now;

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      // Klasifikasi status lingkungan berdasarkan ambang batas 31.0°C[cite: 1]
      String kondisiSuhu = (t <= 31.0) ? "NORMAL" : "PANAS";

      // Penyusunan Payload JSON Telemetri Rutin[cite: 1]
      StaticJsonDocument<256> docOut;
      docOut["node_id"] = clientId;
      docOut["suhu"] = serialized(String(t, 2));
      docOut["kelembapan"] = serialized(String(h, 2));
      docOut["kondisi_suhu"] = kondisiSuhu;
      docOut["status_led"] = digitalRead(ledPin);

      char buffer[256];
      serializeJson(docOut, buffer);
      
      bool success = client.publish(topicPublish, buffer);
      if (success) {
        Serial.printf("[TELEMETRI TERKIRIM] %s\n", buffer);
      } else {
        Serial.println("[ERROR] Gagal memublikasikan telemetri!");
      }

      // Rule Engine & Safety Logic (Suhu > 31.0°C)[cite: 1]
      if (t > 31.0) {
        // Otomatis nyalakan indikator LED darurat[cite: 1]
        digitalWrite(ledPin, HIGH);

        // Kirim alert ke dedicated alert topic jika belum dikirim (mencegah spam)[cite: 1]
        if (!alertSent) {
          StaticJsonDocument<256> docAlert;
          docAlert["status"] = "BAHAYA";
          docAlert["parameter"] = "OVERHEAT";
          docAlert["nilai_suhu"] = serialized(String(t, 2));

          char alertBuffer[256];
          serializeJson(docAlert, alertBuffer);

          bool alertSuccess = client.publish(topicAlert, alertBuffer);
          if (alertSuccess) {
            Serial.printf("[ALERT TERKIRIM] %s\n", alertBuffer);
            alertSent = true; // Kunci latch agar alert tidak berulang-ulang terus menerus
          }
        }
      } else {
        // Reset latch jika suhu sudah kembali normal (<= 31.0°C)
        if (alertSent) {
          alertSent = false;
          Serial.println("[INFO] Suhu kembali normal. Status alert direset.");
        }
      }
    }
  }
}