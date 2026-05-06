#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h> // Aggiunta per il Web Server

// Configurazione Serial1
#define RXD1 14
#define TXD1 15

WebServer server(80);

// Parametri di rete
const char *ssid = "JADE";
const char *password = "Jade#104";
const char *dest_ip = "10.42.0.1"; // Modifica la parte iniziale con la tua subnet
const uint16_t dest_port = 2104;      // Porta di destinazione

WiFiUDP udp;
const unsigned int localPort = 1104;

// Buffers
const size_t MAX_IMAGE_SIZE = 65000;
uint8_t imageBuffer[MAX_IMAGE_SIZE];
size_t currentImageSize = 0;
size_t lastFullImageSize = 0; 
bool isFrameReady = true; // Flag per il web server

char packetBuffer[1500];

void handleRoot() {
  WiFiClient client = server.client();
  
  // 1. Invio degli Header per iniziare uno streaming MJPEG
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println();

  Serial.println("Browser connesso allo streaming");

  while (client.connected()) {
    // Controlliamo se è pronto un nuovo frame
    if (isFrameReady && lastFullImageSize > 0) {
      // 2. Invio del Boundary e dei metadati del frame
      client.println("--frame");
      client.println("Content-Type: image/jpeg");
      client.printf("Content-Length: %d\n\n", lastFullImageSize);
      
      // 3. Invio dei dati binari
      client.write(imageBuffer, lastFullImageSize);
      client.println();
      
      // 4. Reset del flag: aspettiamo che il Core 0 finisca il prossimo frame
      isFrameReady = false; 
    }
    
    // Piccola pausa per non saturare la CPU e permettere ad altri task di girare
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  
  Serial.println("Browser disconnesso");
}

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

  udp.begin(localPort);
  Serial.printf("In ascolto su UDP porta %d\n", localPort);

  server.on("/", handleRoot);
  server.begin();

  // Creiamo il Task sul Core 0
  xTaskCreatePinnedToCore(
    TaskCore0,   /* Funzione */
    "Task0",     /* Nome */
    10000,       /* Stack */
    NULL,        /* Parametri */
    1,           /* Priorità */
    NULL,        /* Task handle */
    0            /* Core 0 */
  );

  // Creiamo il Task sul Core 1
  xTaskCreatePinnedToCore(
    TaskCore1,   /* Funzione */
    "Task1",     /* Nome */
    10000,       /* Stack */
    NULL,        /* Parametri */
    1,           /* Priorità */
    NULL,        /* Task handle */
    1            /* Core 1 */
  );
}

void TaskCore0(void * pvParameters) {
  while(1) {
    int packetSize = udp.parsePacket();
    if (packetSize) {
      int len = udp.read(packetBuffer, sizeof(packetBuffer));
      
      if (len > 0) {
        // 1. Riconoscimento Header Dimensione ("SIZE" + 4 byte = 8 byte)
        if (len == 8 && strncmp(packetBuffer, "SIZE", 4) == 0) {
          // Estraiamo i 4 byte successivi a "SIZE" e convertiamoli in uint32_t
          memcpy(&lastFullImageSize, packetBuffer + 4, 4);
          
          currentImageSize = 0;   // Reset buffer per nuovo frame
          isFrameReady = false;   // Il frame non è più pronto finché non arriva EOF
          
          Serial.printf("\n--- NUOVO FRAME --- Attesi: %d bytes\n", lastFullImageSize);
        } 
        
        // 2. Riconoscimento Fine Frame
        else if (len == 3 && strncmp(packetBuffer, "EOF", 3) == 0) {
          isFrameReady = true;
          Serial.printf("FRAME COMPLETATO. Totale ricevuto: %d\n", currentImageSize);
        } 
        
        // 3. Accumulo dati dell'immagine
        else {
          if (!isFrameReady && (currentImageSize + len <= MAX_IMAGE_SIZE)) {
            memcpy(imageBuffer + currentImageSize, packetBuffer, len);
            currentImageSize += len;
          }
        }
      }
    }
    vTaskDelay(1/portTICK_PERIOD_MS);
  }
}

void TaskCore1(void * pvParameters){
  while(1){
    server.handleClient();

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
    vTaskDelay(5/portTICK_PERIOD_MS);
  }
}

void loop() {
  vTaskDelete(NULL);
}