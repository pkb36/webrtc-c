import os
import time
from datetime import datetime

PROCESS_PATH = "/home/nvidia/webrtc/gstream_manage.py"
START_COMMAND = f"nohup python {PROCESS_PATH} &"
LOG_DIR = "/home/nvidia/webrtc/logs"  # 로그 디렉토리

# Ensure the log directory exists
os.makedirs(LOG_DIR, exist_ok=True)

def get_log_file():
    """Returns the log file path for today's date."""
    today = datetime.now().strftime("%Y-%m-%d")
    return os.path.join(LOG_DIR, f"{today}.log")

def log_message(message):
    """Writes a log message to the file with a Watchdog prefix."""
    log_file = get_log_file()
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with open(log_file, "a") as f:
        f.write(f"[{timestamp}] [Watchdog] {message}\n")

def is_process_running(process_path):
    """Checks if the process is running using ps command."""
    try:
        output = os.popen(f"ps aux | grep '{process_path}' | grep -v 'grep'").read().strip()
        return bool(output)  # If output is not empty, process is running
    except Exception as e:
        log_message(f"Error checking process: {e}")
        return False

def restart_process():
    """Restarts the process and logs the action."""
    log_message(f"{PROCESS_PATH} is not running. Restarting...")
    os.system(START_COMMAND)
    log_message(f"{PROCESS_PATH} has been restarted.")

if __name__ == "__main__":
    log_message("Watchdog script started.")
    while True:
        time.sleep(60)  # Check every 60 seconds
        if not is_process_running(PROCESS_PATH):
            restart_process()
