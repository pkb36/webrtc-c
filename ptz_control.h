#ifndef __PTZ_CONTROL_H__
#define __PTZ_CONTROL_H__

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include "global_define.h"

#define MAX_PTZ_PRESET  12
#define PTZ_POS_SIZE    11
#define MAX_RANCH_POS   32

enum {
  PTZ_NORMAL = 0,
  PTZ_STOP_FAILED,
};

/* PTZ Data Format 
[Flag][PTZ_DATA]
Flag => 0,1 check data setting..
*/

int update_ptz_pos(int index, unsigned char* pos, int auto_ptz_mode);
int is_work_auto_ptz();
void request_auto_move_ptz_stop();

void set_ptz_move_speed(int move, int auto_ptz);
int set_ptz_pos(int id, unsigned char* read_data, int auto_ptz_mode);
int move_ptz_pos(int index, int auto_ptz_move);
int auto_move_ptz(const char* move_seq);
int get_pt_status();

void send_ptz_move_serial_data(const char *s);
gboolean send_ptz_move_cmd(int direction, int ptz_speed);

int update_ranch_pos(int index, unsigned char* ptz_pos, int set_usage);
int set_ranch_pos(int index, unsigned char* read_data);

extern int ptz_err_code;

#endif
