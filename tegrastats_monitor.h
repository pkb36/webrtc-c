// tegrastats_monitor.h
#ifndef TEGRASTATS_MONITOR_H
#define TEGRASTATS_MONITOR_H

#include <glib.h>

typedef struct {
    int ram_used;
    int ram_total;
    int swap_used;
    int swap_total;
    int cpu_usage[6];  // 6개 코어
    int emc_freq;
    int gr3d_freq;
    float aux_temp;
    float cpu_temp;
    float thermal_temp;
    float ao_temp;
    float gpu_temp;
    float pmic_temp;
} TegrastatsInfo;

gboolean parse_tegrastats_line(const char* line, TegrastatsInfo* info);
TegrastatsInfo* get_tegrastats_info();
char* tegrastats_to_json(TegrastatsInfo* info);

#endif