import os
import re

def get_engine_files_from_txt(file_path):
    engine_files = set()
    delete_files = set()
    pattern = re.compile(r'model-engine-file=(\S+)')
    
    with open(file_path, 'r', encoding='utf-8') as file:
        for line in file:
            matches = pattern.findall(line)
            if matches:
                engine_files.add(matches[0])
                if '#' in line:
                    comment_part = line.split('#', 1)[1]
                    comment_match = pattern.search(comment_part)
                    if comment_match:
                        delete_files.add(comment_match.group(1))
    
    return engine_files, delete_files

def delete_unused_engine_files(directory, valid_engine_files, delete_files):
    for file in os.listdir(directory):
        if file.endswith(".engine"):
            if file in delete_files or file not in valid_engine_files:
                file_path = os.path.join(directory, file)
                os.remove(file_path)
                print(f"Deleted: {file}")

if __name__ == "__main__":
    directory = os.getcwd()  # 현재 디렉토리
    txt_files = ["RGB_yoloV7.txt", "Thermal_yoloV7.txt"]
    valid_engine_files = set()
    delete_files = set()
    
    for txt_file in txt_files:
        if os.path.exists(txt_file):
            valid, to_delete = get_engine_files_from_txt(txt_file)
            valid_engine_files.update(valid)
            delete_files.update(to_delete)
    
    delete_unused_engine_files(directory, valid_engine_files, delete_files)
