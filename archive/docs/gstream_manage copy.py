import os
import datetime
import time
import subprocess

work_directory = "/home/nvidia/webrtc"
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

def timely_disk_check():   
    global disk_check_done_flag
    
    if disk_check_right_time() == True:
        if disk_check_done_flag == False:
            exe_command("./disk_check.sh")
            disk_check_done_flag = True
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

    print("Setup completed. Starting disk check scheduler...")
    
    # 디스크 체크만 계속 실행
    while True:
        time.sleep(60)  # 1분마다 체크
        timely_disk_check()