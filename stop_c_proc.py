import psutil

def kill_process_by_name(name):
    # 모든 프로세스를 순회하며 이름이 일치하는 프로세스를 찾습니다.
    for proc in psutil.process_iter(['pid', 'name']):
        try:
            process_name = proc.info['name'].lower()
            # 프로세스 이름이 일치하고 'python'이 포함되지 않은 경우만 종료
            if name in process_name and 'python' not in process_name:
                proc.kill()  # 프로세스를 종료합니다.
                print(f"Process {process_name} (PID: {proc.info['pid']}) has been killed.")
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            pass

# 예제: 'gst' 및 'webrtc' 프로세스를 종료하지만 'python'은 제외합니다.
kill_process_by_name('gst')
kill_process_by_name('webrtc')

