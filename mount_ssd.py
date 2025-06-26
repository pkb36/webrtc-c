import psutil
import os
import datetime
import time
import sys
import threading
import psutil
import subprocess

def command(command):
    result = subprocess.check_output(command, shell=True, text=True)
    return result


def check_device_exist():
    result = command("lspci")
    # print(result)
    str = "Non-Volatile memory controller"
    result = result.find(str)
    if result == -1:
        return False;


def contain_substr(str, substr):
    result = str.find(substr)
    if result == -1:
        return False
    return True


def check_disk():
    result = command("fdisk -l")
    # print(result)
    str = "/dev/nvme0n1"
    result = result.find(str)
    if result == -1:
        return False;


def disk_format():
    path = "/home/nvidia/data"
    result = command("sudo mkfs.ext4 /dev/nvme0n1")
    if not os.path.exists(path):
        cmd = "mkdir " + path
        result = command(cmd)    
    
    
def substring_after(s, delim):
    return s.partition(delim)[2]
    
    
def get_uuid():
    result = command("blkid | grep /dev/nvme0n1")
    result = substring_after(result, "UUID=\"")
    result = result[0:36]
    return result


def get_uuid_from_fstab():
    result = command("cat /etc/fstab")
    result = substring_after(result, "UUID=")
    result = result[0:36]
    return result
    

def replace_word(infile, old_word, new_word):
    if not os.path.isfile(infile):
        print ("Error on replace_word, not a regular file: " + infile)
        sys.exit(1)

    f1 = open(infile, 'r').read()
    f2 = open(infile, 'w')
    m = f1.replace(old_word, new_word)
    f2.write(m)


def replace_uuid_in_fstab():
    old_uuid = get_uuid_from_fstab()
    new_uuid = get_uuid()
    
    if (len(old_uuid) == 36 and len(new_uuid) == 36):
        print("old_uuid=" + old_uuid + ", new_uuid=" + new_uuid)
        if old_uuid != new_uuid:
            replace_word("/etc/fstab", old_uuid, new_uuid)
            print("replace in /etc/fstab was done!")      
        else:
            print("old_uuid and new_uuid was same")


def check_by_df():
    result = command("df")
    if contain_substr(result, "/home/nvidia/data") == True:
        print("SSD disk mount was done!")
        return True
    print("SSD disk mount was not done!")
    return False
    

def start_proc():
    if check_by_df() == True:
        return False
    
    if check_device_exist() == False:
        print('Device does not exist')    
        return False
        
    if check_disk() == False:
        print('Disk check failed')    
        return False

    replace_uuid_in_fstab()    


if __name__ == "__main__":
    start_proc()

