#include "log_wrapper.h"
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static FILE *log_fp = NULL;
static char current_log_filename[256] = "";
static char program_name_global[64] = "";
static guint timer_id = 0;

static void get_current_log_filename(char *filename, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    snprintf(filename, size, 
             "./logs/%04d-%02d-%02d_%s.log",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, 
             program_name_global);
}

// DEBUG와 WARN을 제외하고 파일에 기록하는 커스텀 콜백
static void selective_file_callback(log_Event *ev) {
    // DEBUG와 WARN은 파일에 기록하지 않음
    if (ev->level == LOG_DEBUG || ev->level == LOG_WARN) {
        return;
    }
    
    // 나머지 레벨은 파일에 기록 (새로운 형식)
    char buf[64];
    buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
    fprintf(
        ev->udata, "[%s] [%s] %s:%d: ",
        buf, log_level_string(ev->level), ev->file, ev->line);
    vfprintf(ev->udata, ev->fmt, ev->ap);
    fprintf(ev->udata, "\n");  // 빈 줄 추가로 간격 생성
    fflush(ev->udata);
}

static gboolean check_and_rotate_log_file(gpointer user_data) {
    char new_filename[256];
    get_current_log_filename(new_filename, sizeof(new_filename));
    
    // 현재 파일명과 다르면 새 파일로 변경
    if (strcmp(current_log_filename, new_filename) != 0) {
        log_info("Log file rotating from %s to %s", current_log_filename, new_filename);
        
        // 기존 파일 핸들 닫기
        if (log_fp) {
            fclose(log_fp);
            log_fp = NULL;
        }
        
        // 새 파일 열기
        log_fp = fopen(new_filename, "a");
        if (log_fp) {
            strcpy(current_log_filename, new_filename);
            log_add_callback(selective_file_callback, log_fp, LOG_TRACE);
            log_info("Log file rotated to: %s", new_filename);
        }
    }
    
    return G_SOURCE_CONTINUE; // 타이머 계속 실행
}

void init_logging(const char* program_name) {
    // logs 디렉토리 생성
    system("mkdir -p ./logs");
    
    // ✅ 먼저 프로그램명 저장
    strncpy(program_name_global, program_name, sizeof(program_name_global) - 1);
    program_name_global[sizeof(program_name_global) - 1] = '\0';
    
    // 로그 레벨 설정 (모든 레벨 출력)
    log_set_level(LOG_TRACE);
    
    // 콘솔 출력 활성화 (색상 포함)
    log_set_quiet(false);
    
    // ✅ 새 함수로 파일명 생성
    get_current_log_filename(current_log_filename, sizeof(current_log_filename));
    
    // 파일 출력 추가 (DEBUG, WARN 제외)
    log_fp = fopen(current_log_filename, "a");
    if (log_fp) {
        // 기존의 log_add_fp 대신 커스텀 콜백 사용
        log_add_callback(selective_file_callback, log_fp, LOG_TRACE);
        log_info("Log initialized: %s", current_log_filename);
    } else {
        log_error("Failed to open log file: %s", current_log_filename);
    }
    
    // ✅ 타이머 설정 (60초마다 로그 파일 체크)
    timer_id = g_timeout_add_seconds(60, check_and_rotate_log_file, NULL);
    log_info("Log rotation timer started (check every 60 seconds)");
}

void cleanup_logging(void) {
    if (timer_id > 0) {
        g_source_remove(timer_id);
        timer_id = 0;
    }

    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
}

// 기존 코드와의 호환성을 위한 더미 함수들
void manage_log_file(void) {
    // log.c가 자동으로 관리하므로 아무것도 하지 않음
}

void export_version(const char* name, const char* version, int new_flag) {
    FILE *fp;
    if (new_flag) {
        fp = fopen("version.log", "w+t");
    } else {
        fp = fopen("version.log", "a+t");
    }
    if (fp) {
        fprintf(fp, "%s:%s\n", name, version);
        fclose(fp);
    }
}

int get_time(char *time, int max_len) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    strftime(time, max_len, "%Y-%m-%dT%H:%M:%S", localtime(&tv.tv_sec));
    return (tv.tv_usec / 1000);
}

gboolean is_file(const char *fname) {
    return (access(fname, F_OK) == 0) ? TRUE : FALSE;
}

gboolean is_dir(const char *d) {
    struct stat info;
    if (stat(d, &info) != 0) {
        return FALSE;
    }
    return (info.st_mode & S_IFDIR) ? TRUE : FALSE;
}