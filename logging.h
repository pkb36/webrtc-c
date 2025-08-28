#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <time.h>
#include <glib.h>  // gboolean 타입을 위해 추가

// 로그 레벨 정의
typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO, 
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} LogLevel;

// 로깅 설정 구조체
typedef struct {
    LogLevel level;
    bool quiet;
    bool use_color;
    bool thread_safe;
    bool auto_flush;
    FILE *fp;
    char *log_dir;
    char *program_name;
    size_t max_file_size;  // bytes, 0 = no limit
    int max_files;         // number of rotated files to keep
} LogConfig;

// 기존 glog 매크로들 (호환성 유지)
#define glog_trace(...) logging_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define glog_debug(...) logging_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define glog_info(...)  logging_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define glog_plain(...) logging_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define glog_error(...) logging_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define glog_critical(...) logging_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

// 기존 extern 매크로들 (호환성)
#define extern_glog_trace(...) logging_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_debug(...) logging_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_error(...) logging_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_critical(...) logging_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

// 새로운 매크로들
#define log_trace(...) logging_log(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) logging_log(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  logging_log(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  logging_log(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) logging_log(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) logging_log(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

// 로깅 시스템 함수들
void logging_init(const char* program_name, const char* log_dir);
void logging_cleanup(void);
void logging_log(LogLevel level, const char *file, int line, const char *fmt, ...);

// 설정 함수들
void logging_set_level(LogLevel level);
void logging_set_quiet(bool enable);
void logging_set_color(bool enable);
void logging_set_thread_safe(bool enable);
void logging_set_auto_flush(bool enable);
void logging_set_file_rotation(size_t max_size, int max_files);

// 유틸리티 함수들
const char* logging_level_string(LogLevel level);
const char* logging_level_color(LogLevel level);

// 기존 log_wrapper 호환성 함수들
void init_logging(const char* program_name);
void manage_log_file(void);
void export_version(const char* name, const char* version, int new_flag);
int get_time(char *time, int max_len);
gboolean is_file(const char *fname);
gboolean is_dir(const char *d);

#endif // LOGGING_H