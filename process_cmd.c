#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h> 
#include "gstream_main.h"

static pthread_mutex_t g_proc_mutex;

int check_process(int port)
{
	char *line = NULL;
    size_t len = 0;
    ssize_t read;
	char cmd[512];
	sprintf(cmd, "ps -ef | grep stream_base_port=%d", port);
	FILE* fp = popen(cmd, "r");
	while ((read = getline(&line, &len, fp)) != -1) {
		if(strstr(line, "grep") == 0){
			char *ptr = strtok(line, " ");	
			ptr = strtok(NULL, " ");     //2번째가 pid임 
			int pid = atoi(ptr);
			pclose(fp);
			return pid;
		}
    }
	pclose(fp);
	return -1;
}


int execute_process(char* cmd, gboolean check_id)
{
	glog_trace("process start %s \n", cmd);
	// pid_t pid = fork(); 
	// if(pid == 0){ 
	system(cmd); 
	// 	exit(0);
	// }

	if(check_id){
		int id = check_process(5000);
		return id;
	}
	return 0;
}


void kill_process(int target_pid)
{
	char cmd[512];
	sprintf(cmd, "kill -9 %d", target_pid);
	pid_t pid = fork();
	if(pid ==0){
		system(cmd);
		exit(0);
	}
}


void *internal_process_cmd(void *arg)
{
	FILE* fp;
  	glog_trace("start internal_process_cmd \n");
	char *line = NULL;
    size_t len = 0;
    ssize_t read;

	while(1){
		pthread_mutex_lock(&g_proc_mutex);
		fp  = fopen("procss_status.cfg", "r+t");
		if(fp){
			while ((read = getline(&line, &len, fp)) != -1) {
				char *ptr = strtok(line, ":");	
				if(strcmp(ptr,"REC") ==0){
					ptr = strtok(NULL, ":");     
				}
			}
			fclose(fp);
		}
		pthread_mutex_unlock(&g_proc_mutex);
		sleep(2);
	}

  	glog_trace("endup internal_process_cmd \n");
  	return 0;
}


int	start_process_cmd_loop()
{
	pthread_t tid;
	pthread_mutex_init(&g_proc_mutex, NULL);
	pthread_create(&tid,NULL,internal_process_cmd,(void *)NULL);
	return tid;
}


void stop_process_cmd_loop()
{	
	int status;	
	pthread_t tid = 0;	 
	pthread_join(tid, (void **)&status);
}


/*
int main (int argc, char *argv[])
{
	process_rec(argv[1]);
}
*/

// 	// int ret = check_process(5003);
// 	// glog_trace("check port  porcess %d \n", ret);
// 	// int pid = execute_process(NULL);
// 	// glog_trace("child process id %d \n", pid);
// 	// sleep(5);
// 	// glog_trace("kill process id %d \n", pid);
// 	// kill_process(pid);

// 	// check_process(5003);
// 	pthread_t tid = start_process_cmd_loop();

// 	stop_process_cmd_loop();
// 	return 0;

// }

