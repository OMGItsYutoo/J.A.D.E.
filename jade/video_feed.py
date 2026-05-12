import cv2
from vilib import Vilib
from time import sleep
import socket

ADDR='10.42.0.239'
PORT=1104

#Struttura concettuale del pacchetto UDP
#[1 byte frame ID] [1 byte indice pacchetto] [1 byte totale pacchetto]


def main():
    try:
        # Configurazione Socket UDP
        socket_esp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        socket_esp.bind(('', 3104))
    except Exception as e:
        print(f"\nErrore socket: {e}")
        return
    
    # Avvio fotocamera
    Vilib.camera_start(vflip=False, hflip=False, size=(480,320))
    sleep(1) # Attesa per il riscaldamento del sensore
    
    #Vilib.display(local=False, web=True)
    
    current_frame_id = 0
    while True:
        # 1. Recupera il frame corrente direttamente dalla RAM
        frame = Vilib.img
        
        # Assicurati che il frame sia stato catturato correttamente
        if frame is not None:
            
            small_frame = cv2.resize(frame, (480, 320))
           
            # 2. Comprimi il frame in formato JPEG al volo (in memoria)
            # cv2.imencode restituisce una tupla (successo_operazione, immagine_codificata)
            encode_param = [int(cv2.IMWRITE_JPEG_QUALITY), 50]
            success, encoded_img = cv2.imencode('.jpg', small_frame, encode_param)
            
            if success:
                # 3. Converti l'immagine compressa in una sequenza di byte
                image_data = encoded_img.tobytes()
                chunk_size = 1024
                total_size = len(image_data)

                total_chunks = (total_size + 1400 - 1) // 1400 #1400 è il MAX_UDP_PAYLOAD

                if total_chunks > 255:
                    print(f"Pacchetto troppo grande frammenta di piu, grandezza: {total_chunks}")
                    return
                frame_id_byte = current_frame_id % 256

                print(f"Inizio invio frame: {total_size} bytes")

                sleep(0.001)

                for i in range(total_chunks):
                    start = i * 1400
                    end = min(start + 1400, total_size)
                    chunk_data = image_data[start:end]

                    header = bytes([frame_id_byte, i, total_chunks])
                    packet = header + chunk_data

                    socket_esp.sendto(packet, (ADDR, PORT))
                    sleep(0.004) # Micro-pausa per far respirare il buffer di rete
                
                current_frame_id += 1
                # 5. Marker di fine pacchetto per avvisare l'ESP
                socket_esp.sendto(b'EOF', (ADDR, PORT))
                
        # 6. Pausa per limitare i Frame Per Second (FPS) 
        # Evita di saturare il processore del Pi e la banda Wi-Fi
        sleep(0.05) # ~20 FPS teorici massimi

if __name__=='__main__':
     main()
