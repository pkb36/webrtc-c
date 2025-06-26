#ifndef __GSTREAMER_MAIN_H__
#define __GSTREAMER_MAIN_H__

#include <gst/gst.h>
#include "json_utils.h"
#include "log_wrapper.h"
#include "version.h"

enum AppState
{
  APP_STATE_UNKNOWN = 0,
  APP_STATE_ERROR = 1,          /* generic error */
  APP_STATE_ERROR_TIMEOUT,          /* generic error */
  SERVER_CONNECTING = 1000,
  SERVER_CONNECTION_ERROR,
  SERVER_CONNECTED,             /* Ready to register */
  SERVER_REGISTERING = 2000,
  SERVER_REGISTRATION_ERROR,
  SERVER_REGISTERED,            /* Ready to call a peer */
  SERVER_CLOSED,                /* server connection closed by us or the server */
  ROOM_JOINING = 3000,
  ROOM_JOIN_ERROR,
  ROOM_JOINED,
  ROOM_CALL_NEGOTIATING = 4000, /* negotiating with some or all peers */
  ROOM_CALL_OFFERING,           /* when we're the one sending the offer */
  ROOM_CALL_ANSWERING,          /* when we're the one answering an offer */
  ROOM_CALL_STARTED,            /* in a call with some or all peers */
  ROOM_CALL_STOPPING,
  ROOM_CALL_STOPPED,
  ROOM_CALL_ERROR,
};


// Enum to represent internet connection states
typedef enum 
{
    INIT,
    INTERNET_CONNECTED,
    INTERNET_DISCONNECTED,
    INTERNET_RECONNECTED
} InternetState;

#define MAX_WAIT_REPLY_CNT        5

#define PING_TEST "ping -c 1 8.8.8.8 > /dev/null 2>&1"

void        send_msg_server (const gchar * msg);
//implement process_cmd
int         execute_process(char* cmd, gboolean check_id);

gboolean    process_message_cmd(gJSONObj* jsonObj);

gboolean    apply_setting();
gboolean    cleanup_and_retry_connect (const gchar * msg, enum AppState state);

void        start_heartbit(int timeout);
void        kill_heartbit();

extern gboolean cleanup_and_retry_connect (const gchar * msg, enum AppState state);
extern void send_camera_info_to_server();
extern int is_process_running(const char *process_name);
extern int get_temp(int index);
extern void dec_temp_event_time_gap();

#endif
