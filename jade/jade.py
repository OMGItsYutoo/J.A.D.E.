from picrawler import Picrawler
from time import sleep
import socket

crawler = Picrawler()

SPEED_MIN = 50
SPEED_MAX = 90
speed = 90

STEP = 1            # Number of action steps per key press
ACTION_GAP = 0.05   # Delay after each action to reduce current spikes

def do_move(action_name):
    """Execute movement action with safety delay."""
    crawler.do_action(action_name, STEP, speed)
    print(action_name)
    sleep(ACTION_GAP)

def safe_sit():
    """Safely sit down before program exit."""
    try:
        crawler.do_step("sit", 40)
        sleep(1.0)
    except Exception:
        pass

def main():
    try:

        socket_esp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        socket_esp.bind(('', 2104))

        while True:

            value, address = socket_esp.recvfrom(64) #value <=> xxxx,yyyy
            print(value)
            try:
                valueX,valueY = value.decode("utf8").split(',')
                valueX = int(valueX)
                valueY = int(valueY)

                if valueX >= 2848:
                    do_move("forward")
                elif valueX <= 1248:
                    do_move("backward")
                elif valueY >= 2848:
                    do_move("turn right")
                elif valueY <= 1248:
                    do_move("turn left")
            except:
                print("Invalid data")

            socket_esp.setblocking(False)
            try:
                while True:
                    data = socket_esp.recv(4096)
                    if not data:
                        break 
            except BlockingIOError:
                pass
            socket_esp.setblocking(True)

            sleep(0.02)

    except KeyboardInterrupt:
        print("\nQuit (KeyboardInterrupt).")

    except socket.error:
        print("\nSocket error.")

    finally:
        socket_esp.close()
        safe_sit()

if __name__ == "__main__":
    main()
