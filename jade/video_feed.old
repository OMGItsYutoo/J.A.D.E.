import cv2
from vilib import Vilib
from time import sleep
import socket

ADDR='10.42.0.239'
PORT=1104

def main():
    try:
        # Configurazione Socket UDP
        socket_esp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        socket_esp.bind(('', 3104))
    except Exception as e:
        print(f"\nErrore socket: {e}")
        return
    
    # Avvio fotocamera
    Vilib.camera_start(vflip=False, hflip=False, size=(160,120))
    sleep(1) # Attesa per il riscaldamento del sensore
    
    #Vilib.display(local=False, web=True)
    
    while True:
        # 1. Recupera il frame corrente direttamente dalla RAM
        frame = Vilib.img
        
        # Assicurati che il frame sia stato catturato correttamente
        if frame is not None:
            
            # 2. Comprimi il frame in formato JPEG al volo (in memoria)
            # cv2.imencode restituisce una tupla (successo_operazione, immagine_codificata)
            success, encoded_img = cv2.imencode('.jpg', frame)
            
            if success:
                # 3. Converti l'immagine compressa in una sequenza di byte
                image_data = encoded_img.tobytes()
                chunk_size = 1024
                total_size = len(image_data)

                header_packet = b'SIZE' + total_size.to_bytes(4, 'little')

                print(f"Inizio invio frame: {total_size} bytes")
                socket_esp.sendto(header_packet, (ADDR, PORT))

                sleep(0.001)

                for i in range(0, total_size, chunk_size):
                    chunk = image_data[i : i + chunk_size]
                    socket_esp.sendto(chunk, (ADDR, PORT))
                    sleep(0.002) # Micro-pausa per far respirare il buffer di rete
                    
                # 5. Marker di fine pacchetto per avvisare l'ESP
                socket_esp.sendto(b'EOF', (ADDR, PORT))
                
        # 6. Pausa per limitare i Frame Per Second (FPS) 
        # Evita di saturare il processore del Pi e la banda Wi-Fi
        sleep(0.05) # ~20 FPS teorici massimi

if __name__=='__main__':
     main()
