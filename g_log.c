#include "g_log.h"

#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <json-glib/json-glib.h>

static const char* glog_text[GLOG_MAX] = {"TR", "ER", "CR"};
static char g_program_name[64] ={}; 
static pthread_mutex_t g_log_mutex;

#define MAX_BUF_SIZE 1024
#define LOG_DIR "./logs/"

void get_cur_programname()
{
	char exe_path[MAX_BUF_SIZE];
    int len;

    // 프로세스의 실행 경로 읽기
    len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        perror("Error getting program path");
        return ;
    }
    exe_path[len] = '\0';

    // 경로에서 프로그램 이름 추출
    char *program_name = strrchr(exe_path, '/');
    if (program_name != NULL) {
        program_name++; // '/' 다음 문자부터 프로그램 이름이 시작됨
    } else {
        program_name = exe_path; // '/' 없는 경우 전체 경로가 프로그램 이름임
    }

	strcpy(g_program_name, program_name);
}

void redirect_output()
{
  static FILE *fp1 = NULL;
  static FILE *fp2 = NULL;
  char filename[100] = "";

  puts("Start redirect_output");

  if (fp1) {
    fclose(fp1);
    fp1 = NULL;
  }
  if (fp2) {
    fclose(fp2);
    fp2 = NULL;
  }

  make_dir(LOG_DIR);
  if (get_log_file_name(filename) == 0) {
    printf("get_log_file_name failed!\n");
    return;
  }
  printf("log file name is %s\n", filename);
  
  fflush(stdout);
  fflush(stderr);

  if ((fp1 = freopen(filename, "a+t", stdout)) == NULL) {
      glog_error("freopen() for stdout failed");
	printf("freopen() for stdout failed\n");
	return;
  }
  if ((fp2 = freopen(filename, "a+t", stderr)) == NULL) {
      glog_error("freopen() for stderr failed");
      return;
  }
  printf("stdout and stderr are redirected to \'%s\'\n", filename);
  glog_trace("stdout and stderr are redirected to \'%s\'\n", filename);
}


int get_time(char *time, int max_len)
{
	int msec = 0;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	strftime(time, max_len, "%Y-%m-%dT%H:%M:%S", localtime(&tv.tv_sec));
	msec = (tv.tv_usec / 1000);

	return msec;
}

int get_short_time(char *time, int max_len)
{
	int msec = 0;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	strftime(time, max_len, "%m-%dT%H:%M:%S", localtime(&tv.tv_sec));
	msec = (tv.tv_usec / 1000);

	return msec;
}

int get_log_file_name(char *filename)
{
	char time[36] = "";

	get_time(time, sizeof(time));
	filename[0] = 0;
	strcpy(filename, LOG_DIR);
	strcat(filename, time);
	char *p = strchr(filename, 'T');
	if (p == NULL)
		return 0;
	*p = 0;
	strcat(filename, ".log");

	return strlen(filename);
}


void make_dir(char *name)
{
	struct stat info;

	if (stat(name, &info) != 0) {     // stat 함수가 실패하면 폴더가 존재하지 않는 것으로 간주
		mkdir(name, 0777);
	}
}


gboolean is_file(const char *fname)
{
  if (access(fname, F_OK) == 0) 
    return TRUE;

  return FALSE;
}


gboolean is_dir(const char *d)
{
    DIR *dirptr;

    if (access(d, F_OK) != -1) {
        if ((dirptr = opendir (d)) != NULL) {
            closedir (dirptr); /* d exists and is a directory */
			return TRUE;
        } 
	}

	return FALSE;
}


void file_create(const char *fname)
{
  FILE *fptr;

  fptr = fopen(fname, "w");
  fclose(fptr);
}


void manage_log_file()
{
  char fname[100] = "";
  static char prev_fname[100] = "";

  if (get_log_file_name(fname) > 0) {
    if (is_file(fname) == FALSE)
      file_create(fname);

    if (strcmp(prev_fname, fname) != 0) {
      if (strlen(prev_fname) > 0) {
        redirect_output();
	  }
      strcpy(prev_fname, fname);
    }
  }
}


void glog(int level, int file_append, const char *file, int line, const char *fmt, ...)
{
	static int first = 1;

	if (first) {
		first = 0;
		pthread_mutex_init(&g_log_mutex, NULL);
		make_dir(LOG_DIR);
	}

	pthread_mutex_lock(&g_log_mutex);
	fflush(stdout);
	fflush(stderr);

	char text[5000] = "";
	char time[36] = "";
	char g_szTemp[5000] = {0};  
	char *szTemp = g_szTemp;

	va_list argList;                                                        
	va_start(argList, fmt);    
	vsnprintf(szTemp,sizeof(g_szTemp) -1, fmt, argList);
	va_end(argList);                                                                                

	if (level == GLOG_PLAIN) {
		snprintf(text, sizeof(text), "%s", szTemp);
	}
	else {
		int msec = get_short_time(time, sizeof(time));
		snprintf(text, sizeof(text), "%s:[%s:%d;%s.%03d] %s", glog_text[level], file, line, time, msec, szTemp);
	}

	if (file_append == 0) {
		printf("%s", text);
	} 
	else {
		char filename[100] = "";

		get_log_file_name(filename);
        FILE * fp = fopen(filename,"a+t");
        if(fp) {
                fprintf(fp,"%s", text);
                fclose(fp);
        }
	}

	fflush(stdout);
	fflush(stderr);

	pthread_mutex_unlock(&g_log_mutex);
}


void export_version(const char* name, const char* version, int new_flag)
{
	FILE *fp = 0;
	if(new_flag){
		fp = fopen("version.log", "w+t");
	} else {
		fp = fopen("version.log", "a+t");
	}
	fprintf(fp, "%s:%s\n",name, version);
	fclose(fp); 
}


#ifdef TEST_LOG
#include <unistd.h>
int main(int argc, char *argv[])
{
	glog_trace("tset trace  %d \n", 1);
	usleep(10000);
	glog_trace("tset critical %d \n", 2);

	usleep(50000);

	glog_trace("tset trace  %d \n", 3);
	usleep(100000);
	glog_error("tset error  %d \n", 4);
	usleep(200000);
	glog_critical("tset critical  %d \n", 5);
}
#endif
