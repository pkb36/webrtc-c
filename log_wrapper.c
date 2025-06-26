#include "log_wrapper.h"
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static FILE *log_fp = NULL;

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

void init_logging(const char* program_name) {
    // logs 디렉토리 생성
    system("mkdir -p ./logs");
    
    // 로그 레벨 설정 (모든 레벨 출력)
    log_set_level(LOG_TRACE);
    
    // 콘솔 출력 활성화 (색상 포함)
    log_set_quiet(false);
    
    // 로그 파일 이름 생성 (날짜_프로그램명.log)
    char filename[256];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    
    snprintf(filename, sizeof(filename), 
             "./logs/%04d-%02d-%02d_%s.log",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, 
             program_name);
    
    // 파일 출력 추가 (DEBUG, WARN 제외)
    log_fp = fopen(filename, "a");
    if (log_fp) {
        // 기존의 log_add_fp 대신 커스텀 콜백 사용
        log_add_callback(selective_file_callback, log_fp, LOG_TRACE);
        log_info("Log initialized: %s", filename);
    } else {
        log_error("Failed to open log file: %s", filename);
    }
}

void cleanup_logging(void) {
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