/**
 * Unified Logging System for WebRTC Project
 * Combines functionality from g_log.c, log.c, and log_wrapper.c
 * Thread-safe with file rotation and timestamp support
 */

#include "unified_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <glib.h>

// Global state
static struct {
    FILE *log_file;
    char program_name[64];
    char current_log_filename[256];
    int log_level;
    bool console_enabled;
    bool file_enabled;
    guint timer_id;
    pthread_mutex_t mutex;
    bool initialized;
} log_state = {
    .log_file = NULL,
    .log_level = LOG_TRACE,
    .console_enabled = true,
    .file_enabled = true,
    .timer_id = 0,
    .initialized = false
};

static const char *level_strings[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
    "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"
};
#endif

// Thread-safe logging
static void lock_log(void) {
    if (log_state.initialized) {
        pthread_mutex_lock(&log_state.mutex);
    }
}

static void unlock_log(void) {
    if (log_state.initialized) {
        pthread_mutex_unlock(&log_state.mutex);
    }
}

// Get current timestamp with milliseconds
static int get_timestamp_ms(char *buffer, size_t size, struct tm **tm_ptr) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    struct tm *local_tm = localtime(&tv.tv_sec);
    if (tm_ptr) *tm_ptr = local_tm;
    
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", local_tm);
    return (tv.tv_usec / 1000); // milliseconds
}

// Get program name from /proc/self/exe
static void get_program_name(void) {
    char exe_path[256];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    
    if (len == -1) {
        strcpy(log_state.program_name, "unknown");
        return;
    }
    
    exe_path[len] = '\0';
    char *name = strrchr(exe_path, '/');
    if (name) {
        strncpy(log_state.program_name, name + 1, sizeof(log_state.program_name) - 1);
    } else {
        strncpy(log_state.program_name, exe_path, sizeof(log_state.program_name) - 1);
    }
    log_state.program_name[sizeof(log_state.program_name) - 1] = '\0';
}

// Generate log filename with date and program name
static void get_log_filename(char *filename, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    snprintf(filename, size, "./logs/%04d-%02d-%02d_%s.log",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, 
             log_state.program_name);
}

// Create logs directory
static void ensure_logs_directory(void) {
    struct stat st = {0};
    if (stat("./logs", &st) == -1) {
        mkdir("./logs", 0755);
    }
}

// Timer callback for log file rotation
static gboolean rotate_log_file_timer(gpointer user_data) {
    if (!log_state.file_enabled) return G_SOURCE_CONTINUE;
    
    char new_filename[256];
    get_log_filename(new_filename, sizeof(new_filename));
    
    // Check if we need to rotate
    if (strcmp(log_state.current_log_filename, new_filename) != 0) {
        lock_log();
        
        // Close current file
        if (log_state.log_file) {
            fprintf(log_state.log_file, "\n[Log rotated to: %s]\n", new_filename);
            fclose(log_state.log_file);
        }
        
        // Open new file
        log_state.log_file = fopen(new_filename, "a");
        if (log_state.log_file) {
            strcpy(log_state.current_log_filename, new_filename);
            fprintf(log_state.log_file, "\n[Log started: %s]\n", log_state.program_name);
        }
        
        unlock_log();
    }
    
    return G_SOURCE_CONTINUE;
}

// Initialize unified logging system
void log_init(const char *program_name_override) {
    if (log_state.initialized) return;
    
    // Initialize mutex
    pthread_mutex_init(&log_state.mutex, NULL);
    
    // Get program name
    if (program_name_override) {
        strncpy(log_state.program_name, program_name_override, 
                sizeof(log_state.program_name) - 1);
        log_state.program_name[sizeof(log_state.program_name) - 1] = '\0';
    } else {
        get_program_name();
    }
    
    // Create logs directory
    ensure_logs_directory();
    
    // Open initial log file
    if (log_state.file_enabled) {
        get_log_filename(log_state.current_log_filename, sizeof(log_state.current_log_filename));
        log_state.log_file = fopen(log_state.current_log_filename, "a");
        
        if (log_state.log_file) {
            fprintf(log_state.log_file, "\n=== Log Session Started: %s ===\n", log_state.program_name);
        }
    }
    
    // Start rotation timer (check every minute)
    log_state.timer_id = g_timeout_add_seconds(60, rotate_log_file_timer, NULL);
    
    log_state.initialized = true;
    
    // Log initialization message
    log_write(LOG_INFO, __FILE__, __LINE__, "Unified logging initialized: %s", 
              log_state.current_log_filename);
}

// Cleanup logging system
void log_cleanup(void) {
    if (!log_state.initialized) return;
    
    log_write(LOG_INFO, __FILE__, __LINE__, "Logging system shutting down");
    
    // Remove timer
    if (log_state.timer_id > 0) {
        g_source_remove(log_state.timer_id);
        log_state.timer_id = 0;
    }
    
    // Close log file
    if (log_state.log_file) {
        fprintf(log_state.log_file, "=== Log Session Ended ===\n\n");
        fclose(log_state.log_file);
        log_state.log_file = NULL;
    }
    
    // Destroy mutex
    pthread_mutex_destroy(&log_state.mutex);
    log_state.initialized = false;
}

// Set logging level
void log_set_level(int level) {
    if (level >= LOG_TRACE && level <= LOG_FATAL) {
        log_state.log_level = level;
    }
}

// Enable/disable console output
void log_set_console(bool enable) {
    log_state.console_enabled = enable;
}

// Enable/disable file output
void log_set_file(bool enable) {
    log_state.file_enabled = enable;
}

// Core logging function
void log_write(int level, const char *file, int line, const char *fmt, ...) {
    if (!log_state.initialized) {
        log_init(NULL); // Auto-initialize
    }
    
    if (level < log_state.log_level) return;
    
    va_list args;
    va_start(args, fmt);
    
    lock_log();
    
    // Get timestamp
    char timestamp[32];
    struct tm *tm_info;
    int msec = get_timestamp_ms(timestamp, sizeof(timestamp), &tm_info);
    
    // Extract filename from full path
    const char *basename = strrchr(file, '/');
    if (basename) basename++;
    else basename = file;
    
    // Console output
    if (log_state.console_enabled) {
        char time_short[16];
        strftime(time_short, sizeof(time_short), "%H:%M:%S", tm_info);
        
#ifdef LOG_USE_COLOR
        fprintf(stdout, "%s.%03d %s%-5s\x1b[0m \x1b[90m%s:%d:\x1b[0m ",
                time_short, msec, level_colors[level], level_strings[level], basename, line);
#else
        fprintf(stdout, "%s.%03d %-5s %s:%d: ",
                time_short, msec, level_strings[level], basename, line);
#endif
        vfprintf(stdout, fmt, args);
        fprintf(stdout, "\n");
        fflush(stdout);
    }
    
    // File output (skip DEBUG for file logging to reduce file size)
    if (log_state.file_enabled && log_state.log_file && level != LOG_DEBUG) {
        fprintf(log_state.log_file, "[%s.%03d] [%-5s] %s:%d: ",
                timestamp, msec, level_strings[level], basename, line);
        
        // Reset va_list for file writing
        va_end(args);
        va_start(args, fmt);
        vfprintf(log_state.log_file, fmt, args);
        fprintf(log_state.log_file, "\n");
        fflush(log_state.log_file);
    }
    
    unlock_log();
    va_end(args);
}

// Utility functions for compatibility

// Get current time string
int get_time(char *time_str, int max_len) {
    if (!time_str) return 0;
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    strftime(time_str, max_len, "%Y-%m-%dT%H:%M:%S", localtime(&tv.tv_sec));
    return (tv.tv_usec / 1000);
}

// Check if file exists
gboolean is_file(const char *fname) {
    if (!fname) return FALSE;
    struct stat st;
    return (stat(fname, &st) == 0 && S_ISREG(st.st_mode)) ? TRUE : FALSE;
}

// Check if directory exists
gboolean is_dir(const char *dirname) {
    if (!dirname) return FALSE;
    struct stat st;
    return (stat(dirname, &st) == 0 && S_ISDIR(st.st_mode)) ? TRUE : FALSE;
}

// Create directory
void make_dir(const char *dirname) {
    if (!dirname) return;
    struct stat st = {0};
    if (stat(dirname, &st) == -1) {
        mkdir(dirname, 0755);
    }
}

// Export version information
void export_version(const char *name, const char *version, int new_flag) {
    if (!name || !version) return;
    
    log_write(LOG_INFO, __FILE__, __LINE__, 
              "Version Info - %s: %s %s", 
              name, version, new_flag ? "(NEW)" : "");
    
    // Also write to separate version file
    FILE *vf = fopen("version.log", new_flag ? "w" : "a");
    if (vf) {
        char timestamp[32];
        get_timestamp_ms(timestamp, sizeof(timestamp), NULL);
        fprintf(vf, "[%s] %s: %s\n", timestamp, name, version);
        fclose(vf);
    }
}

// Get log filename (for compatibility)
int get_log_file_name(char *filename) {
    if (!filename) return 0;
    get_log_filename(filename, 256);
    return 1;
}

// Manage log files (cleanup old files)
void manage_log_file(void) {
    // Clean up log files older than 30 days
    system("find ./logs -name '*.log' -mtime +30 -delete 2>/dev/null");
}