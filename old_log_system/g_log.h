#ifndef __LOG_TIMESTAMP_H__
#define __LOG_TIMESTAMP_H__

#include <stdio.h>
#include <json-glib/json-glib.h>

#include "global_define.h"

enum {
  GLOG_TRACE = 0,
  GLOG_ERROR,
  GLOG_CRITICAL,
  GLOG_PLAIN,
  GLOG_DEBUG,  
  GLOG_MAX,
};

void glog(int level, int file_append, const char *file, int line, const char *fmt, ...);

#define glog_trace(...) glog(GLOG_TRACE, 0, __FILE__, __LINE__,  __VA_ARGS__)
#define glog_error(...) glog(GLOG_ERROR, 0, __FILE__, __LINE__, __VA_ARGS__)
#define glog_critical(...) glog(GLOG_CRITICAL, 0, __FILE__, __LINE__, __VA_ARGS__)
#define glog_plain(...) glog(GLOG_PLAIN, 0, __FILE__, __LINE__, __VA_ARGS__)
#define glog_debug(...) glog(GLOG_DEBUG, 0, __FILE__, __LINE__, __VA_ARGS__)

#define extern_glog_trace(...) glog(GLOG_TRACE, 1, __FILE__, __LINE__,  __VA_ARGS__)
#define extern_glog_error(...) glog(GLOG_ERROR, 1, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_critical(...) glog(GLOG_CRITICAL, 1, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_plain(...) glog(GLOG_PLAIN, 1, __FILE__, __LINE__, __VA_ARGS__)
#define extern_glog_debug(...) glog(GLOG_DEBUG, 1, __FILE__, __LINE__, __VA_ARGS__)

void export_version(const char* name, const char* version, int new_flag);
int get_log_file_name(char *filename);
void make_dir(char *name);
void manage_log_file();

extern int get_time(char *time, int max_len);
extern gboolean is_file(const char *fname);
extern gboolean is_dir(const char *d);
#endif	// __LOG_TIMESTAMP_H__
