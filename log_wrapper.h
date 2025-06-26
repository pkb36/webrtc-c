#ifndef LOG_WRAPPER_H
#define LOG_WRAPPER_H

#include "log.h"
#include <stdio.h>
#include <glib.h>  // gboolean 타입을 위해 추가

// 기존 glog 매크로를 log.c로 연결
#define glog_trace(...) log_trace(__VA_ARGS__)
#define glog_error(...) log_error(__VA_ARGS__)
#define glog_critical(...) log_fatal(__VA_ARGS__)
#define glog_info(...) log_info(__VA_ARGS__)
#define glog_plain(...) log_info(__VA_ARGS__)
#define glog_debug(...) log_debug(__VA_ARGS__)

// 기존 함수들과의 호환성을 위해
#define extern_glog_trace(...) log_trace(__VA_ARGS__)
#define extern_glog_error(...) log_error(__VA_ARGS__)
#define extern_glog_critical(...) log_fatal(__VA_ARGS__)
#define extern_glog_debug(...) log_debug(__VA_ARGS__)

// 초기화 함수
void init_logging(const char* program_name);
void cleanup_logging(void);

// 기존 코드와의 호환성을 위한 더미 함수들
void manage_log_file(void);
void export_version(const char* name, const char* version, int new_flag);
int get_time(char *time, int max_len);
gboolean is_file(const char *fname);
gboolean is_dir(const char *d);

#endif