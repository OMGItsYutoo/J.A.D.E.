#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// Configurazione Serial1
#define RXD1 14
#define TXD1 15

// Parametri di rete
const char *ssid = "*****";
const char *password = "******";
const char *dest_ip = "******"; // Modifica la parte iniziale con la tua subnet
const uint16_t dest_port = ****;      // Porta di destinazione

WiFiUDP udp;

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, RXD1, TXD1);

  // Connessione WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connessione al WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connesso!");
  Serial.print("IP locale ESP32: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  if (Serial1.available()) {
    String data = Serial1.readStringUntil('\n');
    
    if (data.length() > 0) {
      Serial.print("Dato ricevuto da STM32: ");
      Serial.println(data);

      // Invio del pacchetto UDP in chiaro al destinatario .140
      udp.beginPacket(dest_ip, dest_port);
      udp.print(data);
      udp.endPacket();
    }
  }

  if (Serial.available()) {
    Serial1.write(Serial.read());
  }
}
