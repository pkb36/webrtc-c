/*
https://documenter.getpostman.com/view/3777989/2s935isRZv#2670a9c7-7027-4d69-b488-7b30c3fddc8d
server ip address
13.230.49.200
*/
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <curl/curl.h>

#include "curllib.h"
#include "json_utils.h"
#include "log_wrapper.h"

struct MemoryStruct {
    char *memory;
    size_t size;
};

// login
/*
// request
    curl --location -g --request POST 'http://{{url}}/api/login/' \
    --data-raw '{
        "email":"pottersd2@naver.com",
        "password":"12341234",
        "fcmToken": ""
    }'
// response
	{
	  "token": "b70cf154882db7992749ee56646fc2a5dcdf31f0",
	  "user": {
	    "id": 1,
	    "email": "pottersd2@naver.com",
	    "name": "�ŵ���"
	  }
	}
*/
static char *login_url = "/api/login/";
static int make_login_url(char *url, CurlIinfoType *j)
{
	char port[32];
	if(j->port > 0)
	{
		sprintf(port, "%d", j->port);
		sprintf(url, "http://%s:%s%s", j->server_ip, port, login_url);				//LJH, server IP is 52.194.238.184
	}
	else
	{
		sprintf(url, "http://%s%s", j->server_ip, login_url);
	}

	if(CURLLIB_DEBUG) printf("%s : %s \n", __func__, url);
	return 0;
}

static int make_login_json(char *json, char *phone, char *password)
{
	sprintf(json, "{\"phone\":\"%s\",\"password\":\"%s\",\"fcmToken\":\"\"}", phone, password);
	if(CURLLIB_DEBUG) printf("%s : %s \n", __func__, json);
	return 0;
}

size_t write_to_memory_callback(void *buffer, size_t size, size_t nmemb, void *userp) {

    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *) userp;
    char *ptr = (char *) realloc(mem->memory, mem->size + realsize + 1);

    if(!ptr) {
        printf("not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), buffer, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
	printf("%s copied %d \n", __func__, (int)mem->size);
    return realsize;
}


static int login_parser(char *val, CurlIinfoType *j)
{
	gJSONObj*  obj = get_json_object(val);
	if(obj == NULL){
		printf("Fail get login parser\n");
		return -1;
	}

	const gchar* token;
	if(!cockpit_json_get_string(obj->object, "token" , NULL, &token, FALSE)){
		free_json_object(obj);
		printf("Fail get login parser in token \n");
		return -1;
	}

	strncpy(j->token, token, 128);
	printf("token : %s \n", j->token);

	free_json_object(obj);
	return 0;
}


int login_request(CurlIinfoType *j)
{    
	char url[256];
	char json[1024];
    CURL *curl;
    // CURLcode res;
    curl = curl_easy_init();
    struct curl_slist *headers = NULL;

    printf("%s + \n", __func__);

    // =========================================
    // METHOD : POST & CALLBACK
    if(curl) {

        headers = NULL;

        struct MemoryStruct chunk;
        chunk.memory = (char *) malloc(1);
        chunk.size = 0;

        // URL
        memset(url, 0, sizeof(url));
        make_login_url(url, j);
        curl_easy_setopt(curl, CURLOPT_URL, url);

        // METHOD
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
//        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");

        // HEADERS
		headers = curl_slist_append(headers, "Accept: application/json");
		headers = curl_slist_append(headers, "Content-Type: application/json");
		headers = curl_slist_append(headers, "charset: utf-8");
		
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // SSL
//        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
//        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1L);

        // DATA
        memset(json, 0, sizeof(json));
        make_login_json(json, j->phone, j->password);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
        // CALLBACK
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_memory_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &chunk);

        // EXECUTE
        // res = curl_easy_perform(curl);
		curl_easy_perform(curl);

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
#if 1
		printf("chunk size %d \n", (int)chunk.size);
        if (chunk.size > 0) {
            printf("chunk data : %s \n", chunk.memory);
        }
#endif
		login_parser(chunk.memory, j);
		        
        free(chunk.memory);
    }
    printf("%s - \n", __func__);
    return 0;
}


// notification
/*
// request
	curl --location -g --request POST 'http://{{url}}/api/notification/create/' \
	--header 'Authorization: Token 28d563b43c1b8a6c1c70db558d12c7d1a9e4d2e1' \
	--data-raw '{
	    "camera": 10,
	    "notificationCategory": 3
	}'

//response
	{
	  "id": 13,
	  "notificationCategory": {
	    "id": 3,
	    "name": "��� ����"
	  },
	  "is_checked": false,
	  "created_at": "2023-01-31T10:06:06.328528+09:00"
	}
*/
static char *notification_url = "/api/notification/create/";
static int make_notification_url(char *url, CurlIinfoType *j)
{
	char port[32];
	if(j->port > 0)
	{
		sprintf(port, "%d", j->port);
		sprintf(url, "http://%s:%s%s", j->server_ip, port, notification_url);
	}
	else
	{
		sprintf(url, "http://%s%s", j->server_ip, notification_url);
	}

	if(CURLLIB_DEBUG) printf("%s : %s \n", __func__, url);
	return 0;
}


size_t write_return_data(void *buffer, size_t size, size_t nmemb, void *userp)
{
   FILE *fp = fopen("event_result.html", "w+t");
   fwrite(buffer,  size,  nmemb, fp);
   fclose(fp);
   return size * nmemb;
}


void set_snapshot_path(CurlIinfoType *j)
{
//LJH, j->snapshot_path was set by config
	if (strstr(j->video_url, "CAM0")) {
        strcpy(j->snapshot_path, "/home/nvidia/webrtc/cam0_snapshot.jpg");
    } else if (strstr(j->video_url, "CAM1")) {
        strcpy(j->snapshot_path, "/home/nvidia/webrtc/cam1_snapshot.jpg");
    } else {
        strcpy(j->snapshot_path, "/home/nvidia/webrtc/cam0_snapshot.jpg");
    }
}

int notification_request(char *cam, char *evt, CurlIinfoType *j)
{
    CURL *curl;
    CURLcode res;
    char url[256];
    char hdr[256];

    curl_mime *form = NULL;
    curl_mimepart *field = NULL;
    struct curl_slist *headerlist = NULL;

    // 로그인 요청 처리 (토큰이 비어 있으면 로그인)
    if (j->token[0] == 0)
        login_request(j);

    printf("%s cam %s evt %s %s + \n", __func__, cam, evt, j->token);

	curl_global_init(CURL_GLOBAL_ALL);
	curl = curl_easy_init();
	if(curl) {
		form = curl_mime_init(curl);
	
		field = curl_mime_addpart(form);
		if (!field) { glog_error("Failed to add camera part.\n"); goto cleanup; }
		curl_mime_name(field, "camera");
		curl_mime_data(field, cam, CURL_ZERO_TERMINATED);
	
		field = curl_mime_addpart(form);
		if (!field) { glog_error("Failed to add notificationCategory part.\n"); goto cleanup; }
		curl_mime_name(field, "notificationCategory");
		curl_mime_data(field, evt, CURL_ZERO_TERMINATED);
		glog_trace("evt=%s\n", evt);
	
		set_snapshot_path(j);
	
		field = curl_mime_addpart(form);
		if (!field) { glog_error("Failed to add image part.\n"); goto cleanup; }
		curl_mime_name(field, "image");
		curl_mime_filedata(field, j->snapshot_path);
	
		field = curl_mime_addpart(form);
		if (!field) { glog_error("Failed to add video_url part.\n"); goto cleanup; }
		curl_mime_name(field, "video_url");
		curl_mime_data(field, j->video_url, strlen(j->video_url));
		glog_trace("video_url=%s(len=%d), snapshot_path=%s(len=%d)\n",
				   j->video_url, strlen(j->video_url), j->snapshot_path, strlen(j->snapshot_path));
	
		if (j->position[0] != 0) {
			field = curl_mime_addpart(form);
			if (!field) { glog_error("Failed to add position part.\n"); goto cleanup; }
			curl_mime_name(field, "position");
			curl_mime_data(field, j->position, CURL_ZERO_TERMINATED);
			glog_trace("position=%s\n", j->position);
		}
	
		// Set headers
		memset(hdr, 0, sizeof(hdr));
		sprintf(hdr, "Authorization: Token %s", j->token);
		headerlist = curl_slist_append(headerlist, hdr);
	
		// Set URL and options	
		memset(url, 0, sizeof(url));
		make_notification_url(url, j);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);
		curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_return_data);
	
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	
		res = curl_easy_perform(curl);
		if (res != CURLE_OK)
			glog_error("curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
		usleep(100);

		if (j->position[0] != 0) {
			memset(j->position, 0, sizeof(j->position));
		}
		memset(j->video_url, 0, sizeof(j->video_url));

	cleanup:
		curl_mime_free(form);
		if (headerlist) curl_slist_free_all(headerlist);
		curl_easy_cleanup(curl);
	}
	else {
		glog_error("Failed to initialize curl.\n");
        return -1;
    }

    return 0;
}
