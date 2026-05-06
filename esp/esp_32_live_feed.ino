#include <WiFi.h>
#include <WiFiUdp.h>

// --- CONFIGURAZIONE RETE ---
const char* ssid = "JADE";
const char* password = "Jade#104";

IPAddress local_IP(10, 42, 0, 239); 
IPAddress gateway(10, 42, 0, 1);    
IPAddress subnet(255, 255, 255, 0);

WiFiUDP udp;
const int udpPort = 1104;
WiFiServer server(80);

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

void setup() {
  Serial.begin(115200);
  
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("Errore configurazione IP Statico");
  }

  // Inizializza i buffer
  for (int i=0; i<NUM_FRAMES; i++) {
    ring_buffer[i].ready = false;
    ring_buffer[i].size = 0;
  }

  Serial.print("\nConnessione a ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWi-Fi Connesso!");
  Serial.print("Indirizzo IP ESP32: ");
  Serial.println(WiFi.localIP());

  udp.begin(udpPort);
  Serial.printf("In ascolto UDP su porta %d\n", udpPort);

  server.begin();
  Serial.println("Server HTTP avviato sulla porta 80");
}

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

void loop() {
  handleUDP();

  WiFiClient client = server.available();
  if (client) {
    Serial.println("Browser PC connesso!");
    
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: multipart/x-mixed-replace; boundary=myboundary");
    client.println("Access-Control-Allow-Origin: *");
    client.println();
    
    while (client.connected()) {
      handleUDP(); // Dreniamo l'hardware continuamente
      
      // --- CONTROLLO DEL CONSUMER ---
      // Se nel vassoio di lettura c'è un frame pronto, lo serviamo
      if (ring_buffer[read_idx].ready) {
        
        client.print("--myboundary\r\n");
        client.print("Content-Type: image/jpeg\r\n");
        client.printf("Content-Length: %u\r\n\r\n", ring_buffer[read_idx].size);
        
        // Invio via TCP (questa è la parte lenta che il buffer sta mitigando)
        client.write(ring_buffer[read_idx].data, ring_buffer[read_idx].size);
        client.print("\r\n");
        
        // "Svuotiamo" il vassoio
        ring_buffer[read_idx].ready = false;
        
        // Avanziamo il puntatore di lettura
        read_idx = (read_idx + 1) % NUM_FRAMES;
      }
      
      delay(1); 
    }
    Serial.println("Client disconnesso.");
  }
}