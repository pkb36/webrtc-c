#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include "json_utils.h"

// 함수 포인터 타입 정의
typedef void (*send_message_func_t)(const char* message);

// 함수 선언
void handle_custom_command(gJSONObj* jsonObj, send_message_func_t send_func);
void send_command_result_to_websocket(const char* command, const char* result, const char* peer_id, send_message_func_t send_func);

// 개별 명령어 실행 함수들 (필요시 외부에서 호출 가능)
char* execute_safe_command(const char* command);
char* execute_sudo_command(const char* command);
char* parse_tegrastats_output(void);

#endif // COMMAND_HANDLER_H