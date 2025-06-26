import psutil

def kill_process_by_name(name):
    # 모든 프로세스를 순회하며 이름이 일치하는 프로세스를 찾습니다.
    for proc in psutil.process_iter(['pid', 'name']):
        try:
            # 프로세스 이름이 일치하는 경우
            if name in proc.info['name']  :
                proc.kill()  # 프로세스를 종료합니다.
                print(f"Process {name} (PID: {proc.info['pid']}) has been killed.")
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass

# 예제: 'notepad.exe' 프로세스를 종료합니다.
kill_process_by_name('gst')
kill_process_by_name('webrtc')
kill_process_by_name('python')