#ifndef __CURLLIB_H__
#define __CURLLIB_H__


#ifdef __cplusplus
extern "C"  //C++
{
#endif

#define CURLLIB_DEBUG 1

typedef struct 
{
    char phone[32];
    char password[32];
    char token[128];
    char snapshot_path[256];
    char video_url[256];
    char position[30];                  //LJH, 1129
    char server_ip[32];
    int  port;
} CurlIinfoType;

int login_request(CurlIinfoType *j);
int camera_request(CurlIinfoType *j);
int camera_mac_request(CurlIinfoType *j);
int notification_request(char *cam, char *evt, CurlIinfoType *j);

#ifdef __cplusplus
}
#endif

#endif
