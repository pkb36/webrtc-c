
// tegrastats_monitor.c
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include "log_wrapper.h"
#include "tegrastats_monitor.h"

// tegrastats 출력 파싱 함수 - 수정된 버전
gboolean parse_tegrastats_line(const char* line, TegrastatsInfo* info) {
    // 디버깅: 원본 라인 출력
    glog_trace("Parsing tegrastats line: %s\n", line);
    
    // 초기화
    memset(info, 0, sizeof(TegrastatsInfo));
    
    // RAM 파싱: "RAM 5847/6849MB"
    char* ram_pos = strstr(line, "RAM ");
    if (ram_pos) {
        if (sscanf(ram_pos, "RAM %d/%dMB", &info->ram_used, &info->ram_total) != 2) {
            glog_error("Failed to parse RAM info\n");
            return FALSE;
        }
        glog_trace("RAM: %d/%dMB\n", info->ram_used, info->ram_total);
    } else {
        glog_error("RAM info not found\n");
        return FALSE;
    }
    
    // SWAP 파싱: "SWAP 716/3424MB"
    char* swap_pos = strstr(line, "SWAP ");
    if (swap_pos) {
        if (sscanf(swap_pos, "SWAP %d/%dMB", &info->swap_used, &info->swap_total) != 2) {
            glog_error("Failed to parse SWAP info\n");
            // SWAP은 선택사항이므로 계속 진행
        } else {
            glog_trace("SWAP: %d/%dMB\n", info->swap_used, info->swap_total);
        }
    }
    
    // CPU 사용률 파싱: "CPU [42%@1420,35%@1420,...]"
    char* cpu_pos = strstr(line, "CPU [");
    if (cpu_pos) {
        cpu_pos += 5; // "CPU [" 길이
        
        // 각 CPU 코어 파싱
        for (int i = 0; i < 6; i++) {
            int usage;
            int freq;
            
            if (sscanf(cpu_pos, "%d%%@%d", &usage, &freq) == 2) {
                info->cpu_usage[i] = usage;
                glog_trace("CPU[%d]: %d%%\n", i, usage);
            } else {
                glog_error("Failed to parse CPU[%d]\n", i);
                break;
            }
            
            // 다음 코어로 이동
            cpu_pos = strchr(cpu_pos, ',');
            if (!cpu_pos || i == 5) break;  // 마지막 코어이거나 더 이상 없으면
            cpu_pos++; // ',' 건너뛰기
        }
    } else {
        glog_error("CPU info not found\n");
        return FALSE;
    }
    
    // 주파수 파싱: "EMC_FREQ 0% GR3D_FREQ 39%"
    char* emc_pos = strstr(line, "EMC_FREQ ");
    if (emc_pos) {
        sscanf(emc_pos, "EMC_FREQ %d%%", &info->emc_freq);
    }
    
    char* gr3d_pos = strstr(line, "GR3D_FREQ ");
    if (gr3d_pos) {
        sscanf(gr3d_pos, "GR3D_FREQ %d%%", &info->gr3d_freq);
    }
    
    // 온도 파싱 - 정수와 소수점 모두 지원
    char* temp_pos;
    
    // AUX 온도
    if ((temp_pos = strstr(line, "AUX@")) != NULL) {
        if (sscanf(temp_pos, "AUX@%fC", &info->aux_temp) != 1) {
            // 정수로 다시 시도
            int temp;
            if (sscanf(temp_pos, "AUX@%dC", &temp) == 1) {
                info->aux_temp = (float)temp;
            }
        }
        glog_trace("AUX temp: %.1f°C\n", info->aux_temp);
    }
    
    // CPU 온도
    if ((temp_pos = strstr(line, "CPU@")) != NULL) {
        if (sscanf(temp_pos, "CPU@%fC", &info->cpu_temp) != 1) {
            int temp;
            if (sscanf(temp_pos, "CPU@%dC", &temp) == 1) {
                info->cpu_temp = (float)temp;
            }
        }
        glog_trace("CPU temp: %.1f°C\n", info->cpu_temp);
    }
    
    // thermal 온도
    if ((temp_pos = strstr(line, "thermal@")) != NULL) {
        if (sscanf(temp_pos, "thermal@%fC", &info->thermal_temp) != 1) {
            int temp;
            if (sscanf(temp_pos, "thermal@%dC", &temp) == 1) {
                info->thermal_temp = (float)temp;
            }
        }
        glog_trace("Thermal temp: %.1f°C\n", info->thermal_temp);
    }
    
    // AO 온도
    if ((temp_pos = strstr(line, "AO@")) != NULL) {
        if (sscanf(temp_pos, "AO@%fC", &info->ao_temp) != 1) {
            int temp;
            if (sscanf(temp_pos, "AO@%dC", &temp) == 1) {
                info->ao_temp = (float)temp;
            }
        }
        glog_trace("AO temp: %.1f°C\n", info->ao_temp);
    }
    
    // GPU 온도
    if ((temp_pos = strstr(line, "GPU@")) != NULL) {
        if (sscanf(temp_pos, "GPU@%fC", &info->gpu_temp) != 1) {
            int temp;
            if (sscanf(temp_pos, "GPU@%dC", &temp) == 1) {
                info->gpu_temp = (float)temp;
            }
        }
        glog_trace("GPU temp: %.1f°C\n", info->gpu_temp);
    }
    
    // PMIC 온도
    if ((temp_pos = strstr(line, "PMIC@")) != NULL) {
        if (sscanf(temp_pos, "PMIC@%fC", &info->pmic_temp) != 1) {
            int temp;
            if (sscanf(temp_pos, "PMIC@%dC", &temp) == 1) {
                info->pmic_temp = (float)temp;
            }
        }
        glog_trace("PMIC temp: %.1f°C\n", info->pmic_temp);
    }
    
    return TRUE;
}

// tegrastats 실행하고 결과 가져오기
TegrastatsInfo* get_tegrastats_info() {
    static TegrastatsInfo info;
    memset(&info, 0, sizeof(TegrastatsInfo));
    
    // CPU 사용률 읽기 (/proc/stat)
    FILE* fp = fopen("/proc/stat", "r");
    if (fp) {
        char line[256];
        int cpu_idx = 0;
        
        while (fgets(line, sizeof(line), fp) && cpu_idx < 6) {
            if (strncmp(line, "cpu", 3) == 0 && line[3] >= '0' && line[3] <= '5') {
                // 간단한 CPU 사용률 계산 (실제로는 더 복잡함)
                info.cpu_usage[cpu_idx] = 50;  // 임시값
                cpu_idx++;
            }
        }
        fclose(fp);
    }
    
    // 메모리 정보 읽기 (/proc/meminfo)
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[256];
        int mem_total = 0, mem_free = 0, mem_available = 0;
        
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "MemTotal: %d kB", &mem_total) == 1) {
                info.ram_total = mem_total / 1024;
            } else if (sscanf(line, "MemAvailable: %d kB", &mem_available) == 1) {
                info.ram_used = (mem_total - mem_available) / 1024;
            }
        }
        fclose(fp);
    }
    
    // 온도 정보 읽기 (thermal zone)
    int thermal_zones[] = {0, 1};  // CPU, GPU
    float* temps[] = {&info.cpu_temp, &info.gpu_temp};
    
    for (int i = 0; i < 2; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/virtual/thermal/thermal_zone%d/temp", thermal_zones[i]);
        
        fp = fopen(path, "r");
        if (fp) {
            int temp;
            if (fscanf(fp, "%d", &temp) == 1) {
                *temps[i] = temp / 1000.0;  // millidegree to degree
            }
            fclose(fp);
        }
    }
    
    return &info;
}

// JSON 형태로 변환
char* tegrastats_to_json(TegrastatsInfo* info) {
    static char json_buffer[2048];
    
    snprintf(json_buffer, sizeof(json_buffer),
        "{"
        "\"timestamp\": %ld,"
        "\"memory\": {"
        "  \"ram_used\": %d,"
        "  \"ram_total\": %d,"
        "  \"ram_percentage\": %.1f,"
        "  \"swap_used\": %d,"
        "  \"swap_total\": %d,"
        "  \"swap_percentage\": %.1f"
        "},"
        "\"cpu\": {"
        "  \"cores\": [%d, %d, %d, %d, %d, %d],"
        "  \"average\": %.1f"
        "},"
        "\"temperature\": {"
        "  \"cpu\": %.1f,"
        "  \"gpu\": %.1f,"
        "  \"thermal\": %.1f,"
        "  \"aux\": %.1f,"
        "  \"ao\": %.1f,"
        "  \"pmic\": %.1f"
        "}"
        "}",
        time(NULL),
        info->ram_used, info->ram_total, 
        (float)info->ram_used / info->ram_total * 100,
        info->swap_used, info->swap_total,
        (float)info->swap_used / info->swap_total * 100,
        info->cpu_usage[0], info->cpu_usage[1], info->cpu_usage[2],
        info->cpu_usage[3], info->cpu_usage[4], info->cpu_usage[5],
        (float)(info->cpu_usage[0] + info->cpu_usage[1] + info->cpu_usage[2] + 
                info->cpu_usage[3] + info->cpu_usage[4] + info->cpu_usage[5]) / 6,
        info->cpu_temp, info->gpu_temp, info->thermal_temp,
        info->aux_temp, info->ao_temp, info->pmic_temp
    );
    
    return json_buffer;
}