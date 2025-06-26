#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include "video_convert.h"


void change_extension(const char* filename, const char* new_extension, char* new_filename) 
{
    // Find the position of the last '.' in the filename
    const char *dot = strrchr(filename, '.');

    // If there is no extension, return the original filename
    if (!dot) {
        strcpy(new_filename, filename);  // Copy the filename as is if no extension
        return;
    }

    // Copy the part of the filename before the extension
    size_t len = dot - filename;  // Length up to the '.' character

    // Append the new extension to the existing filename (without the old extension)
    strncpy(new_filename, filename, len);
    new_filename[len] = '\0';  // Null-terminate the string

    // Add the new extension
    strcat(new_filename, new_extension);
}


int convert_webm_to_mp4(int delay_time, char *filename)
{
    char command[512];

    memset(command, 0, sizeof(command));
    sprintf(command, "./video_convert.sh %d %s &", delay_time, filename);
    glog_trace("command=%s\n", command);
    int result = system(command);
    if (result != 0) {
        glog_trace("system call '%s' failed\n", command);
        return -1;
    }

	return 0;
}


// Function to get the file creation time
time_t get_file_creation_time(const char *file_path) 
{
    struct stat st;
    if (stat(file_path, &st) == 0) {
        return st.st_ctime; // st_ctime is the file creation time
    }
    return -1; // Return -1 if an error occurs
}

#define MAX_PROCESS_NAME_LENGTH 256

// Function to check if a process name matches the provided name
int count_processes_by_name(const char *process_name) 
{
    DIR *dir;
    struct dirent *entry;
    int count = 0;
    char path[512];
    FILE *fp;
    char cmdline[MAX_PROCESS_NAME_LENGTH];

    // Open the /proc directory to iterate through the processes
    dir = opendir("/proc");
    if (dir == NULL) {
        perror("opendir");
        return -1;
    }

    // Iterate through the directory entries in /proc
    while ((entry = readdir(dir)) != NULL) {
        // Check if the directory entry is a process (i.e., a number)
        if (entry->d_type == DT_DIR && atoi(entry->d_name) > 0) {
            // Build the path to the process's cmdline file
            snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);

            // Open the cmdline file of the process
            fp = fopen(path, "r");
            if (fp == NULL) {
                continue;  // Skip if we can't open the cmdline file
            }

            // Read the cmdline (the command that started the process)
            if (fgets(cmdline, sizeof(cmdline), fp) != NULL) {
                // Compare the cmdline with the process name
                if (strstr(cmdline, process_name) != NULL) {
                    count++;  // Increment if the process name matches
                }
            }
            fclose(fp);
        }
    }

    closedir(dir);
    return count;
}


// Function to find .webm files older than 1 minute in the specified directory and convert them
void process_old_webm_files(const char *directory, int g_duration) 
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    time_t current_time = time(NULL);
    int num = 0;

    if (dir == NULL) {
        glog_error("Unable to open directory\n");
        return;
    }

    // Loop through all the files in the directory
    while ((entry = readdir(dir)) != NULL) {
        // Check if the file ends with .webm
        if (strstr(entry->d_name, ".webm") != NULL) {
            char file_path[512];
            snprintf(file_path, sizeof(file_path), "%s/%s", directory, entry->d_name);

            // Get the file creation time
            time_t creation_time = get_file_creation_time(file_path);
            if (creation_time == -1) {
                glog_error("Error getting creation time for: %s\n", file_path);
                continue;
            }
            num = count_processes_by_name("ffmpeg");
            glog_trace("ffmpeg num=%d\n", num);
            if (num == 0) {
                // Convert files that are older than 10 minute
                if (difftime(current_time, creation_time) > ((g_duration * 60) + 600)) {
                    glog_trace("Found old file: %s, converting...\n", file_path);
                    convert_webm_to_mp4(10, file_path);
                    break;
                }
            }
            else {
                break;
            }
        }
    }

    closedir(dir);
}
