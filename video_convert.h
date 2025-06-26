#ifndef __VIDEO_CONVERT_H__
#define __VIDEO_CONVERT_H__

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include "log_wrapper.h"
#include "global_define.h"

extern int convert_webm_to_mp4(int delay_time, char *filename);
extern void change_extension(const char* filename, const char* new_extension, char* new_filename);
extern void process_old_webm_files(const char *directory, int g_duration);
extern int count_processes_by_name(const char *process_name);
#endif
