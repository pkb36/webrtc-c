/* this source file is for managing the disk space usage */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "log_wrapper.h"

#define MAX_DELETE_COUNT 	50

enum {
	SUCCESS = 0,
	FAIL = 1,
};

int count_digits(const char *line)
{
	int count = 0, i;
	int len = strlen(line);

	for (i = 0; i < len; i++) {
		if (isdigit(line[i]))
			count++;
	}
	
	return count;
}


int get_exec_result(const char *command, char *result, int maxlen)		
{
	FILE *fp;
	int len = 0;
	char line[300];

	fp = popen(command, "r");
	if (fp == NULL) {
		extern_glog_error("Failed to run \'%s\'\n", command);
		return -1;
	}
	memset(line, 0, sizeof(line));
	memset(result, 0, maxlen);
	while (fgets(line, sizeof(line), fp) != NULL)
	{
		len = strlen(result);
		if ((len + strlen(line)) >= maxlen)
			break;
		strcat(&result[len], line);
		memset(line, 0, sizeof(line));
	}
	pclose(fp);
	len = strlen(result);
	if (len > 0 && isspace(result[len-1]))
	{
		result[len-1] = 0;
		len--;
	}
	
	return len;
}


int get_disk_usage(const char *path)
{
	char result[1000];
	char command[300];
	int ret = 0;

	snprintf(command, sizeof(command), "df %s | grep -v Use | awk '{print $5}'", path);
	if (get_exec_result(command, result, sizeof(result)) > 0){
		ret = atoi(result);
		return ret;
	}

	return -1;
}


int get_oneline(char *str, char *line, int line_num)
{
	int count = 0;
	char *temp_str = NULL;
	char *ptr = NULL;
	
	if(line_num < 1) {
		extern_glog_error("line_num should be 1 or bigger\n");
		return -1;
	}
	temp_str = strdup(str);
	ptr = strtok(temp_str, "\n");
	while (ptr != NULL) {
		count++;
		if (line_num == count) {
			strcpy(line, ptr);	
			break;
		}
		ptr = strtok(NULL, "\n");
	}
	free(temp_str);

	return strlen(line);
}


int delete_one_dir(const char *path)
{
	char result[1000];
	char line[200];
	char dirname[10][200];
	char command[300];
	int max_dirname = sizeof(dirname)/sizeof(dirname[0]);
	int str_len = 0, line_num, ret = FAIL, i = 0, dirname_count = 0;

	memset(command, 0, sizeof(command));
	snprintf(command, sizeof(command), "ls %s -tr | head -%d", path, max_dirname);	//get max lines for 'ls [PATH] -tr' result
	if(get_exec_result(command, result, sizeof(result)) <= 0) 
		return FAIL;

	for (i = 0; i < max_dirname; i++)
		memset(dirname[i], 0, sizeof(dirname[i]));

	line_num = 1;
	memset(line, 0, sizeof(line));
	while(dirname_count < max_dirname && get_oneline(result, line, line_num++) > 0)
	{
		if (count_digits(line) == 8)		//ex) 20240808
			strcpy(dirname[dirname_count++], line);
		memset(line, 0, sizeof(line));
	}
	if (dirname_count == 0)
	{
		extern_glog_trace("dirname count is 0\n");
		return FAIL;
	}

	for (i = 0; i < dirname_count; i++)
	{
		memset(command, 0, sizeof(command));
		snprintf(command, sizeof(command), "bash -i -c \'rm -rf %s/%s 2>&1\'", path, dirname[i]);
		str_len = get_exec_result(command, result, sizeof(result));
		if (str_len == 0) {
			extern_glog_trace("\'%s\' was done\n", command);
			ret = SUCCESS;
			break;
		}
		else {
			extern_glog_error("Error: %s\n", result);
		}
	}
	
	return ret;
}


void make_space(char *path, int trigger_point, int deletion)
{
	int count = 0, usage = 0;
	int target = trigger_point - deletion;
	
	if ((usage = get_disk_usage(path)) < trigger_point) { 	
		extern_glog_trace("The usage is %d, that is lower than trigger_point(=%d)\n", usage, trigger_point);
		return;
	}
	if (trigger_point < deletion) {
		extern_glog_trace("deletion percentage is higher than trigger_point\n");
		return;
	}
	while ((usage = get_disk_usage(path)) > target) {								
		if (delete_one_dir(path) == FAIL) {
			extern_glog_error("Failed deletion of a directory\n");
			break;
		}
		if (count++ > MAX_DELETE_COUNT)
			break;
		sleep(2);
	}
}


int main( int argc, char *argv[] )
{
	char disk_path[300];
	int trigger_point = 0;
	int deletion = 0;
	
	if (argc == 4) {
		strcpy(disk_path, argv[1]);
		trigger_point = atoi(argv[2]);
		deletion = atoi(argv[3]);
	}
	else {
		printf("Usage : %s disk_path trigger_point(%%) deletion(%%)\n", argv[0]);
		return 1;
	}
	make_space(disk_path, trigger_point, deletion);

	return 0;
}
