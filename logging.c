#include "logging.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

// 전역 로깅 설정
static LogConfig g_config = {
    .level = LOG_TRACE,
    .quiet = false,
    .use_color = true,
    .thread_safe = true,
    .auto_flush = true,
    .fp = NULL,
    .log_dir = NULL,
    .program_name = NULL,
    .max_file_size = 10 * 1024 * 1024,  // 10MB
    .max_files = 5
};

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;

// 색상 코드
static const char* level_colors[] = {
    "\x1b[94m", // TRACE - Light Blue
    "\x1b[36m", // DEBUG - Cyan  
    "\x1b[32m", // INFO  - Green
    "\x1b[33m", // WARN  - Yellow
    "\x1b[31m", // ERROR - Red
    "\x1b[35m"  // FATAL - Magenta
};

static const char* level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

// 유틸리티 함수들
const char* logging_level_string(LogLevel level) {
    return level_names[level];
}

const char* logging_level_color(LogLevel level) {
    return level_colors[level];
}

// 로그 디렉토리 생성
static void create_log_directory(const char* dir) {
    if (!dir) return;
    
    struct stat st = {0};
    if (stat(dir, &st) == -1) {
        mkdir(dir, 0755);
    }
}

// 현재 날짜로 로그 파일명 생성
static void get_log_filename(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    
    if (g_config.log_dir) {
        snprintf(buffer, size, "%s/%s_%04d%02d%02d.log", 
                g_config.log_dir,
                g_config.program_name ? g_config.program_name : "app",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    } else {
        snprintf(buffer, size, "%s_%04d%02d%02d.log",
                g_config.program_name ? g_config.program_name : "app", 
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    }
}

// 파일 크기 확인
static size_t get_file_size(FILE* fp) {
    if (!fp) return 0;
    
    long pos = ftell(fp);
    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    return size;
}

// 로그 파일 로테이션
static void rotate_log_file(void) {
    if (!g_config.fp || g_config.max_files <= 0) return;
    
    // 현재 파일 크기 확인
    if (get_file_size(g_config.fp) < g_config.max_file_size) return;
    
    fclose(g_config.fp);
    g_config.fp = NULL;
    
    char current_file[512];
    get_log_filename(current_file, sizeof(current_file));
    
    // 기존 로테이션 파일들 이동
    for (int i = g_config.max_files - 1; i > 0; i--) {
        char old_file[512], new_file[512];
        snprintf(old_file, sizeof(old_file), "%s.%d", current_file, i - 1);
        snprintf(new_file, sizeof(new_file), "%s.%d", current_file, i);
        rename(old_file, new_file);
    }
    
    // 현재 파일을 .0으로 이동
    char backup_file[512];
    snprintf(backup_file, sizeof(backup_file), "%s.0", current_file);
    rename(current_file, backup_file);
    
    // 새 파일 열기
    g_config.fp = fopen(current_file, "a");
}

// 로그 파일 열기/생성
static void open_log_file(void) {
    if (g_config.fp) return;
    
    char filename[512];
    get_log_filename(filename, sizeof(filename));
    
    g_config.fp = fopen(filename, "a");
    if (!g_config.fp) {
        fprintf(stderr, "Failed to open log file: %s\n", filename);
    }
}

// 로깅 초기화
void logging_init(const char* program_name, const char* log_dir) {
    if (g_config.thread_safe) {
        pthread_mutex_lock(&g_mutex);
    }
    
    if (g_initialized) {
        if (g_config.thread_safe) {
            pthread_mutex_unlock(&g_mutex);
        }
        return;
    }
    
    // 프로그램 이름 저장
    if (program_name) {
        free(g_config.program_name);
        g_config.program_name = strdup(program_name);
    }
    
    // 로그 디렉토리 저장
    if (log_dir) {
        free(g_config.log_dir);
        g_config.log_dir = strdup(log_dir);
        create_log_directory(log_dir);
    }
    
    // 로그 파일 열기
    open_log_file();
    
    // 터미널 색상 지원 확인
    g_config.use_color = isatty(STDERR_FILENO);
    
    g_initialized = true;
    
    if (g_config.thread_safe) {
        pthread_mutex_unlock(&g_mutex);
    }
    
    // 시작 메시지
    logging_log(LOG_INFO, __FILE__, __LINE__, "Logging system initialized");
}

// 로깅 정리
void logging_cleanup(void) {
    if (g_config.thread_safe) {
        pthread_mutex_lock(&g_mutex);
    }
    
    if (g_config.fp && g_config.fp != stdout && g_config.fp != stderr) {
        fclose(g_config.fp);
        g_config.fp = NULL;
    }
    
    free(g_config.program_name);
    free(g_config.log_dir);
    g_config.program_name = NULL;
    g_config.log_dir = NULL;
    
    g_initialized = false;
    
    if (g_config.thread_safe) {
        pthread_mutex_unlock(&g_mutex);
    }
}

// 메인 로깅 함수
void logging_log(LogLevel level, const char *file, int line, const char *fmt, ...) {
    if (level < g_config.level) return;
    
    if (g_config.thread_safe) {
        pthread_mutex_lock(&g_mutex);
    }
    
    // 자동 초기화
    if (!g_initialized) {
        logging_init("webrtc", "/home/nvidia/webrtc/logs");
    }
    
    // 로그 파일 로테이션 확인
    if (g_config.max_file_size > 0) {
        rotate_log_file();
    }
    
    // 로그 파일이 없으면 다시 열기
    if (!g_config.fp) {
        open_log_file();
    }
    
    // 타임스탬프 생성
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    
    // 파일명에서 경로 제거
    const char* filename = strrchr(file, '/');
    if (filename) filename++;
    else filename = file;
    
    // 로그 메시지 포맷팅
    va_list args;
    va_start(args, fmt);
    
    char message[1024];
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    
    // 콘솔 출력 (quiet 모드가 아닐 때)
    if (!g_config.quiet) {
        FILE* output = (level >= LOG_ERROR) ? stderr : stdout;
        
        if (g_config.use_color && isatty(fileno(output))) {
            fprintf(output, "%s[%04d-%02d-%02d %02d:%02d:%02d] %-5s\x1b[0m \x1b[90m%s:%d\x1b[0m %s\n",
                   logging_level_color(level),
                   t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                   t->tm_hour, t->tm_min, t->tm_sec,
                   logging_level_string(level),
                   filename, line, message);
        } else {
            fprintf(output, "[%04d-%02d-%02d %02d:%02d:%02d] %-5s %s:%d %s\n",
                   t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                   t->tm_hour, t->tm_min, t->tm_sec,
                   logging_level_string(level),
                   filename, line, message);
        }
        
        if (g_config.auto_flush) {
            fflush(output);
        }
    }
    
    // 파일 출력
    if (g_config.fp) {
        fprintf(g_config.fp, "[%04d-%02d-%02d %02d:%02d:%02d] %-5s %s:%d %s\n",
               t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
               t->tm_hour, t->tm_min, t->tm_sec,
               logging_level_string(level),
               filename, line, message);
        
        if (g_config.auto_flush) {
            fflush(g_config.fp);
        }
    }
    
    if (g_config.thread_safe) {
        pthread_mutex_unlock(&g_mutex);
    }
}

// 설정 함수들
void logging_set_level(LogLevel level) {
    g_config.level = level;
}

void logging_set_quiet(bool enable) {
    g_config.quiet = enable;
}

void logging_set_color(bool enable) {
    g_config.use_color = enable;
}

void logging_set_thread_safe(bool enable) {
    g_config.thread_safe = enable;
}

void logging_set_auto_flush(bool enable) {
    g_config.auto_flush = enable;
}

void logging_set_file_rotation(size_t max_size, int max_files) {
    g_config.max_file_size = max_size;
    g_config.max_files = max_files;
}

// 기존 log_wrapper 호환성 함수들
void init_logging(const char* program_name) {
    logging_init(program_name, "/home/nvidia/webrtc/logs");
}

void manage_log_file(void) {
    // 기존 코드와의 호환성을 위한 더미 함수
}

void export_version(const char* name, const char* version, int new_flag) {
    if (name && version) {
        logging_log(LOG_INFO, __FILE__, __LINE__, "%s version: %s", name, version);
    }
}

int get_time(char *time_str, int max_len) {
    if (!time_str || max_len <= 0) return -1;
    
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    
    return snprintf(time_str, max_len, "%04d-%02d-%02d %02d:%02d:%02d",
                   t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                   t->tm_hour, t->tm_min, t->tm_sec);
}

gboolean is_file(const char *fname) {
    if (!fname) return FALSE;
    
    struct stat st;
    if (stat(fname, &st) == 0) {
        return S_ISREG(st.st_mode) ? TRUE : FALSE;
    }
    return FALSE;
}

gboolean is_dir(const char *d) {
    if (!d) return FALSE;
    
    struct stat st;
    if (stat(d, &st) == 0) {
        return S_ISDIR(st.st_mode) ? TRUE : FALSE;
    }
    return FALSE;
}