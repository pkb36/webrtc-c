import os
import time
import subprocess
from stat import ST_CTIME

def get_file_creation_time(file_path):
    """Get the creation time of a file."""
    try:
        stats = os.stat(file_path)
        return stats[ST_CTIME]
    except FileNotFoundError:
        print(f"Error: File '{file_path}' not found.")
        return None

def reboot_system():
    """Reboot the Linux system."""
    try:
        print("Rebooting the system...")
        subprocess.run(['sudo', 'reboot'], check=True)
    except Exception as e:
        print(f"Error: Failed to reboot the system. {e}")

if __name__ == "__main__":
    file_path = "/home/nvidia/webrtc/cam1_snapshot.jpg"  # Replace with the file you want to monitor

    while True:
        time.sleep(900)
        creation_time = get_file_creation_time(file_path)

        if creation_time is None:
            exit(1)

        current_time = time.time()
        print(f"File creation time: {time.ctime(creation_time)}")
        print(f"Current time: {time.ctime(current_time)}")

        # Check if the creation time differs by more than one minute
        if abs(current_time - creation_time) > 60:
            print("File creation time mismatch exceeds one minute. System will reboot.")
            reboot_system()
#        else:
#            print("File creation time is within the acceptable range. No action required.")

