import os
import datetime
import time
import subprocess
import glob

work_directory = "/home/nvidia/webrtc"
logs_directory = "/home/nvidia/webrtc/logs"
disk_check_hhmm = (3, 0)
disk_check_done_flag = False

def get_cur_hhmm():
    from datetime import datetime    
    now = datetime.now()
    return now.hour, now.minute

def disk_check_right_time():
   global disk_check_hhmm 
   cur_hhmm = get_cur_hhmm()
   if cur_hhmm == disk_check_hhmm:
    return True
   else:
    return False

def cleanup_old_logs():
    """30일 이상 된 로그 파일들을 삭제"""
    try:
        current_date = datetime.datetime.now()
        cutoff_date = current_date - datetime.timedelta(days=30)
        
        # logs 디렉토리의 모든 .log 파일 찾기
        log_pattern = os.path.join(logs_directory, "*.log")
        log_files = glob.glob(log_pattern)
        
        deleted_count = 0
        for log_file in log_files:
            try:
                # 파일명에서 날짜 추출
                filename = os.path.basename(log_file)
                
                # 두 가지 형식 지원: 2025-04-01.log, 2025-06-12_gstream_main.log
                date_str = None
                
                if filename.count('-') >= 2:  # 날짜 형식 확인
                    # 확장자 제거
                    name_without_ext = filename.split('.')[0]
                    
                    # YYYY-MM-DD 패턴 찾기
                    import re
                    date_match = re.match(r'(\d{4}-\d{2}-\d{2})', name_without_ext)
                    if date_match:
                        date_str = date_match.group(1)
                
                if date_str:
                    file_date = datetime.datetime.strptime(date_str, "%Y-%m-%d")
                    
                    # 30일 이상 된 파일 삭제
                    if file_date < cutoff_date:
                        os.remove(log_file)
                        print(f"Deleted old log file: {filename}")
                        deleted_count += 1
                
            except (ValueError, OSError) as e:
                print(f"Error processing log file {log_file}: {e}")
                continue
        
        if deleted_count > 0:
            print(f"Cleaned up {deleted_count} old log files")
        else:
            print("No old log files to clean up")
    
    except Exception as e:
        print(f"Error during log cleanup: {e}")
        
def timely_disk_check():   
    global disk_check_done_flag
    
    if disk_check_right_time() == True:
        if disk_check_done_flag == False:
            print("Starting daily maintenance...")
            exe_command("./disk_check.sh")
            cleanup_old_logs()  # 로그 정리 추가
            disk_check_done_flag = True
            print("Daily maintenance completed")
    else:
        disk_check_done_flag = False

def exe_command(command):
    subprocess.call(command, shell=True)

def change_mode():
    exe_command("chmod u+x gstream_main webrtc_* disk_check *.sh cam_ctl")

def install_tracker_lib():
    exe_command("./install_tracker_lib.sh")

if __name__ == "__main__":
    # DISPLAY 환경변수 제거
    if 'DISPLAY' in os.environ:
       del os.environ['DISPLAY']

    os.chdir(work_directory)
    change_mode()    
    install_tracker_lib()

    # logs 디렉토리가 없으면 생성
    if not os.path.exists(logs_directory):
        os.makedirs(logs_directory)
        print(f"Created logs directory: {logs_directory}")

    print("Setup completed. Starting disk check scheduler...")
    
    # 디스크 체크만 계속 실행
    while True:
        time.sleep(60)  # 1분마다 체크
        timely_disk_check()