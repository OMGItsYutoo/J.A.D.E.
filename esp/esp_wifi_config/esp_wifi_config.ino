#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// Configurazione Serial1
#define RXD1 14
#define TXD1 15

// Parametri di rete
const char *ssid = "JADE";
const char *password = "Jade#104";
const char *dest_ip = "10.42.0.1"; // Modifica la parte iniziale con la tua subnet
const uint16_t dest_port = 2104;      // Porta di destinazione

WiFiUDP udp;
const unsigned int localPort = 1104;

// --- CONFIGURAZIONE RING BUFFER ---
#define NUM_FRAMES 3
#define MAX_JPEG_SIZE 25000 // 40KB per frame per non esaurire la RAM dell'ESP32

// Struttura che rappresenta un singolo "vassoio"
typedef struct {
  uint8_t data[MAX_JPEG_SIZE];
  size_t size;
  bool ready; 
} FrameBuffer;

FrameBuffer ring_buffer[NUM_FRAMES];

// Puntatori logici per la gestione circolare
volatile uint8_t write_idx = 0; // Dove scrive l'UDP (Producer)
volatile uint8_t read_idx = 0;  // Da dove legge il Web Server (Consumer)

// Variabili per l'assemblaggio del frame in ricezione
volatile size_t expected_total_size = 0;
uint8_t current_receiving_frame_id = 255;
uint8_t received_chunks_count = 0;
uint8_t expected_total_chunks = 0;
bool chunk_received[256] = {false};

void handleUDP() {
  int packetSize = udp.parsePacket();
  
  while (packetSize > 0) {
    if (packetSize == 3) {
      char eof_marker[4];
      udp.read(eof_marker, 3);
      eof_marker[3] = '\0';
      
      if (strcmp(eof_marker, "EOF") == 0) {
        
        if (received_chunks_count == expected_total_chunks && expected_total_chunks > 0) {
          // --- FRAME VALIDO COMPLETO ---
          // Lo blocchiamo nel ring buffer e lo segniamo come pronto
          ring_buffer[write_idx].size = expected_total_size;
          ring_buffer[write_idx].ready = true;
          
          // Avanziamo il puntatore di scrittura al prossimo slot
          write_idx = (write_idx + 1) % NUM_FRAMES;
          
          // Se la scrittura ha raggiunto la lettura (Buffer Pieno/Overflow)
          // Forziamo l'avanzamento della lettura per sovrascrivere il frame più vecchio
          if (write_idx == read_idx) {
            read_idx = (read_idx + 1) % NUM_FRAMES;
            Serial.println("Buffer pieno: sovrascrittura frame vecchio.");
          }
        } else {
          Serial.printf("Glitch evitato. Ricevuti %d su %d\n", received_chunks_count, expected_total_chunks);
        }
      }
    } 
    else if (packetSize > 3) {
      uint8_t header[3];
      udp.read(header, 3);
      
      uint8_t frame_id = header[0];
      uint8_t chunk_idx = header[1];
      uint8_t total_chunks = header[2];

      if (frame_id != current_receiving_frame_id) {
        current_receiving_frame_id = frame_id;
        received_chunks_count = 0;
        expected_total_chunks = total_chunks;
        expected_total_size = 0;
        memset(chunk_received, false, sizeof(chunk_received)); 
      }
      
      int payload_len = packetSize - 3;
      int offset = chunk_idx * 1400; 
      
      // Scriviamo i dati direttamente nello slot corrente del ring_buffer
      if (offset + payload_len < MAX_JPEG_SIZE) {
        udp.read(&ring_buffer[write_idx].data[offset], payload_len);
        
        if (!chunk_received[chunk_idx]) {
          chunk_received[chunk_idx] = true;
          received_chunks_count++;
        }
        
        if (offset + payload_len > expected_total_size) {
            expected_total_size = offset + payload_len;
        }
      }
    }
    packetSize = udp.parsePacket(); 
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(2000000, SERIAL_8N1, RXD1, TXD1);

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

  while(1){
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
    vTaskDelay(10/portTICK_PERIOD_MS);
  }
}

void TaskCore1(void * pvParameters){

  static size_t bytes_sent = 0;
  const size_t CHUNK_TX_SIZE = 1024;

  while(1) {
    
      handleUDP(); // Dreniamo l'hardware continuamente
      
      // --- CONTROLLO DEL CONSUMER ---
      // Se nel vassoio di lettura c'è un frame pronto, lo serviamo
      if (ring_buffer[read_idx].ready) {

        size_t bytes_left = ring_buffer[read_idx].size - bytes_sent;

        size_t current_chunk_size = (bytes_left > CHUNK_TX_SIZE) ? CHUNK_TX_SIZE : bytes_left;

        size_t byte_inviati = Serial1.write(ring_buffer[read_idx].data+bytes_sent, current_chunk_size);
        if(byte_inviati != ring_buffer[read_idx].size) {
          Serial.println("Errore trasmissione UART di JPEG!");
        }

        bytes_sent += current_chunk_size;

        if (bytes_sent >= ring_buffer[read_idx].size) {
          // "Svuotiamo" il vassoio
          ring_buffer[read_idx].ready = false;
          
          // Avanziamo il puntatore di lettura
          read_idx = (read_idx + 1) % NUM_FRAMES;

          bytes_sent = 0;
        }
      }

    vTaskDelay(1/portTICK_PERIOD_MS);
  }

}

void loop() {
  vTaskDelete(NULL);
}