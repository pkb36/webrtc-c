#include "curllib.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

void send_msg_server (const char * msg) {
    // 빈 함수 - 필요시 구현
}

const char* GetIP() {
    FILE *fp;
    static char line[256];  // static으로 선언하여 함수 종료 후에도 유지
    
    fp = fopen("local_ip.log", "r+t");
    if(NULL == fp){
        printf ("can not get local ip log\n");
        return NULL;
    }
    
    fgets(line, sizeof(line), fp);
    fgets(line, sizeof(line), fp);
    fclose(fp);
    
    // 개행 문자 제거
    line[strcspn(line, "\n")] = 0;
    
    return line;
}

int main (int argc, char *argv[]) {
    CurlIinfoType j;
    
    // 구조체 초기화
    memset(&j, 0, sizeof(CurlIinfoType));
    
    // 기본 정보 설정
    strcpy(j.phone, "itechour");
    strcpy(j.password, "12341234");
    strcpy(j.server_ip, "52.194.238.184");
    j.port = 0;
    
    // 스냅샷 경로 설정 (실제 존재하는 파일로)
    strcpy(j.snapshot_path, "/home/nvidia/webrtc/cam0_snapshot.jpg");
    
    // 비디오 URL 설정 
    sprintf(j.video_url, "http://121.67.120.198:9515/data/EVENT_20250707/CAM0_172229.mp4");
    printf("video url: %s\n", j.video_url);
    
    // 위치 정보 설정 (선택사항)
    strcpy(j.position, "Test Location");
    
    printf("=== 로그인 시도 ===\n");
    if(login_request(&j) == 0){
        printf("Login successful, token: %s\n", j.token);
        
        printf("\n=== 알림 전송 시도 ===\n");
        printf("Camera ID: 0\n");
        printf("Event Type: 3\n");
        printf("Video URL: %s\n", j.video_url);
        printf("Snapshot Path: %s\n", j.snapshot_path);
        
        // 실제 notification_request 호출
        int result = notification_request("0", "3", &j);
        
        if(result == 0) {
            printf("Notification sent successfully!\n");
        } else {
            printf("Failed to send notification\n");
        }
        
        // 결과 파일 확인
        FILE *result_fp = fopen("event_result.html", "r");
        if(result_fp) {
            printf("\n=== 서버 응답 ===\n");
            char buffer[1024];
            while(fgets(buffer, sizeof(buffer), result_fp)) {
                printf("%s", buffer);
            }
            fclose(result_fp);
        }
        
    } else {
        printf("Login failed\n");
    }
    
    return 0;
}

// 컴파일 명령어:
// gcc test_notification.c curllib.c json_utils.c log_wrapper.c -lcurl -ljson-glib-1.0 -lglib-2.0 -o test_notification

// 실행 전 확인사항:
// 1. /home/nvidia/webrtc/cam0_snapshot.jpg 파일이 존재하는지 확인
// 2. local_ip.log 파일이 존재하는지 확인 (GetIP 함수 사용시)
// 3. 네트워크 연결 상태 확인