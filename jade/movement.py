from picrawler import Picrawler
from time import sleep
import socket

crawler = Picrawler()

SPEED_MIN = 50
SPEED_MAX = 90
speed = 90
deadzone = 1200

STEP = 1            # Number of action steps per key press
ACTION_GAP = 0.05   # Delay after each action to reduce current spikes


def wave_hand(spider):
    coords = [
        # stand
        [[45, 45, -50], [45, 0, -50], [45, 0, -50], [45, 45, -50]],
        # wave hand
        [[45, 45, -70], [60, 0, 120], [45, 0, -60], [45, 45, -30]],
        [[45, 45, -70], [-20, 60, 120], [45, 0, -60], [45, 45, -30]],
        [[45, 45, -70], [60, 0, 120], [45, 0, -60], [45, 45, -30]],
        [[45, 45, -70], [-20, 60, 120], [45, 0, -60], [45, 45, -30]],
        # return to stand
        [[45, 45, -50], [45, 0, -30], [45, 0, -50], [45, 45, -50]],
        [[45, 45, -50], [45, 0, -40], [45, 0, -50], [45, 45, -50]],
        [[45, 45, -50], [45, 0, -50], [45, 0, -50], [45, 45, -50]],
     ]

    for coord in coords:
        spider.do_step(coord, 60)



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
        last_move = ""

        while True:

            value, address = socket_esp.recvfrom(64) #value <=> xxxx,yyyy
            #print(value)
            try:
                valueX,valueY = value.decode("utf8").split(',')
                if((" " in valueX) or (" " in valueY)):
                    raise Exception("")
                valueX = int(valueX)
                valueY = int(valueY)

                if valueX == 0 and valueY == 0:
                    wave_hand(crawler)
                    last_move = ""

                elif valueX >= (2048 + deadzone) and valueX <= 4095:
                    print(value)
                    if(last_move == "f"):
                        do_move("forward")
                    last_move = "f"
                elif valueX <= (2048 - deadzone) and valueX >= 0:
                    print(value)
                    if(last_move == "b"):
                        do_move("backward")
                    last_move = "b"  
                elif valueY >= (2048 + deadzone) and valueY <= 4095:
                    if(last_move == "r"):
                        do_move("turn right")
                    last_move = "r"
                    print(value)
                elif valueY <= (2048 - deadzone) and valueY >= 0:
                    if(last_move == "l"):
                        do_move("turn left")
                    last_move = "l"
                    print(value)
            except:
                print("Invalid data")
                last_move = ""

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
