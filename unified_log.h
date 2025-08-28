/**
 * Unified Logging System Header
 * Replaces g_log.h, log.h, and log_wrapper.h
 * Thread-safe logging with file rotation and multiple output formats
 */

#ifndef UNIFIED_LOG_H
#define UNIFIED_LOG_H

#include <stdio.h>
#include <stdbool.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

// Log levels (compatible with both old systems)
typedef enum {
    LOG_TRACE = 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

// Core logging functions
void log_init(const char *program_name_override);
void log_cleanup(void);
void log_set_level(int level);
void log_set_console(bool enable);
void log_set_file(bool enable);
void log_write(int level, const char *file, int line, const char *fmt, ...);

// Modern logging macros (recommended)
#define log_trace(...) log_write(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define log_info(...)  log_write(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define log_warn(...)  log_write(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define log_error(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define log_fatal(...) log_write(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

// Legacy g_log compatibility macros
#define glog_trace(...) log_write(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define glog_error(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define glog_critical(...) log_write(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)
#define glog_plain(...) log_write(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define glog_debug(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define glog_info(...) log_write(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)

// External logging macros (for file output)
#define extern_glog_trace(...) log_write(LOG_TRACE, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_error(...) log_write(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_critical(...) log_write(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_plain(...) log_write(LOG_INFO, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_debug(...) log_write(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)

// Utility functions (for compatibility with existing code)
int get_time(char *time_str, int max_len);
gboolean is_file(const char *fname);
gboolean is_dir(const char *dirname);
void make_dir(const char *dirname);
void export_version(const char *name, const char *version, int new_flag);
int get_log_file_name(char *filename);
void manage_log_file(void);

// Legacy compatibility (deprecated, but maintained for existing code)
#define GLOG_TRACE LOG_TRACE
#define GLOG_ERROR LOG_ERROR
#define GLOG_CRITICAL LOG_FATAL
#define GLOG_PLAIN LOG_INFO
#define GLOG_DEBUG LOG_DEBUG
#define GLOG_MAX (LOG_FATAL + 1)

// For code that uses the old glog() function directly
static inline void glog(int level, int file_append, const char *file, int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    
    // Map old levels to new levels
    int new_level;
    switch(level) {
        case 0: new_level = LOG_TRACE; break;    // GLOG_TRACE
        case 1: new_level = LOG_ERROR; break;    // GLOG_ERROR  
        case 2: new_level = LOG_FATAL; break;    // GLOG_CRITICAL
        case 3: new_level = LOG_INFO; break;     // GLOG_PLAIN
        case 4: new_level = LOG_DEBUG; break;    // GLOG_DEBUG
        default: new_level = LOG_INFO; break;
    }
    
    // Note: file_append parameter is ignored in unified system
    // All file logging is handled automatically
    log_write(new_level, file, line, fmt, args);
    va_end(args);
}

// Auto-initialization macro (call this once in main())
#define LOG_INIT(program_name) log_init(program_name)
#define LOG_CLEANUP() log_cleanup()

#ifdef __cplusplus
}
#endif

#endif // UNIFIED_LOG_H