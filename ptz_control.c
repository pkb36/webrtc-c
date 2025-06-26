/* For json config */
#include <stdio.h>
#include "serial_comm.h"
#include <pthread.h>
#include <unistd.h> 
#include <termios.h> 
#include <fcntl.h>
#include <sys/select.h>
#include <string.h>
#include <stdlib.h>
#include "log_wrapper.h"
#include <json-glib/json-glib.h>
#include "device_setting.h"
#include "serial_comm.h"
#include "nvds_process.h"

pthread_mutex_t g_motion_mutex;

//Format flag (On/Off), time(stay time), Pos count, [POS...]	
static unsigned char PTZ_POS_LIST[MAX_PTZ_PRESET][PTZ_POS_SIZE];              //ptz_preset in json

static unsigned char AUTO_PTZ_MOVE_SEQ[MAX_PTZ_PRESET+3] = {0};               //index of AUTO_PTZ_POS_LIST 의 list, +3 ==> 앞의 3개는 on/off, stay time, setting 된 AUTO_PTZ_POS_LIST 의 index list 의 count, 그 후로는 AUTO_PTZ_POS_LIST 의 index list
static unsigned char AUTO_PTZ_POS_LIST[MAX_PTZ_PRESET][PTZ_POS_SIZE];         //LJH, actual position, auto_ptz_preset in json

#if MINDULE_INCLUDE
static unsigned char RANCH_POS_LIST[MAX_RANCH_POS][PTZ_POS_SIZE];     //LJH, actual position, ranch_pos in json
#endif

static unsigned char auto_ptz_move_speed = 0x08;
static unsigned char ptz_move_speed = 0x10;

extern DeviceSetting g_setting;
extern int stop_retry_count;
extern int g_notifier_running;

int g_no_zoom = 0;
int ptz_err_code = PTZ_NORMAL;
int g_move_speed;
int g_preset_index = 0;

void set_ptz_move_speed(int move, int auto_ptz)
{
    auto_ptz_move_speed = auto_ptz;
    ptz_move_speed = move;
}


int update_ptz_pos(int index, unsigned char *ptz_pos, int auto_ptz_mode)
{
    if(index >= MAX_PTZ_PRESET){
        return -1;
    }
    if(auto_ptz_mode){
        memcpy(&AUTO_PTZ_POS_LIST[index][1], ptz_pos, 10);
        AUTO_PTZ_POS_LIST[index][0] = 1;                
    } else {
        memcpy(&PTZ_POS_LIST[index][1], ptz_pos, 10);
        PTZ_POS_LIST[index][0] = 1;    
    }
    
    glog_trace("update_ptz_pos auto_ptz_mode[%d] : index[%d] ", auto_ptz_mode, index);
    print_serial_data(ptz_pos, 10);
    
    return 0;
}

unsigned char get_checksum(unsigned char *data, int len)
{
  unsigned char checksum = 0;

  for(int i = 0; i < len; i++)
    checksum += data[i];
  checksum %= 256;

  return checksum;
}

#if MINDULE_INCLUDE

#define RANCH_PTZ_SPEED    100

static gchar* ranch_setting_template_string_json = "{\n\
   \"ranch_pos\": [\n\
   %s\
   ]\n\
}";

gboolean update_ranch_setting(const char *file_name, RanchSetting* setting)
{
  //1. make PTZ code
  char ptz_code[(MAX_RANCH_POS + 2) * PTZ_POS_SIZE *4] = {0};
  char temp[8];

  for(int i = 0 ; i < MAX_RANCH_POS ; i++){
    sprintf(temp, "\"%02x", setting->ranch_pos[i][0]);
    strcat(ptz_code,temp);
    for(int j = 1 ; j < PTZ_POS_SIZE ; j++){
      sprintf(temp, ",%02x",setting->ranch_pos[i][j]);
      strcat(ptz_code,temp);
    }
    if (i < MAX_RANCH_POS - 1) 
      strcat(ptz_code,"\",\n");
    else 
      strcat(ptz_code,"\"\n");
  }

  //2. update config
  FILE *fp = fopen(file_name, "w+t, ccs=UTF-8");
  if(fp == NULL){
    glog_trace("fail open device setting  %s \n", file_name);  
    return FALSE;
  }

  fprintf(fp, ranch_setting_template_string_json, ptz_code);
  fclose(fp);

  return TRUE; 
}


int set_ranch_pos(int index, unsigned char* read_data)
{
    if(!is_open_serial()){
      glog_error("serial device not opened before\n");
      return -1;
    }
    request_auto_move_ptz_stop();

    unsigned char cmd_data[7] = {0x96,0x00,0x06,0x01,0x01,0x01,0x9F};           //LJH, command is 0x0106 = Get All Position
    int result = read_cmd_timeout(cmd_data, 7, read_data, 17, 1);               //LJH, response data length is 11, whole response size is 17
    if(result == -1){
      glog_error("fail read set_ptz_pos \n");
      return result;
    }
    update_ranch_pos(index, &read_data[5], 1);
    return 0;
}


int update_ranch_pos(int index, unsigned char *ptz_pos, int set_usage)
{
    RANCH_POS_LIST[index][0] = set_usage;                
    if (ptz_pos == NULL)
      return 0;
    
    memcpy(&RANCH_POS_LIST[index][1], ptz_pos, 10);
    glog_trace("update_ranch_pos index[%d] ", index);
    print_serial_data(ptz_pos, 10);
    
    return 0;
}


int move_ranch_pos(int index)
{
    if(!is_open_serial()){
        glog_error("serial device not opened before \n");
        return -1;
    }

    if(index >= MAX_RANCH_POS){
        glog_error("PTZ index[%d] exceed Max[%d]\n", index, MAX_RANCH_POS);
        return -1;
    }
    
    int is_set_berfore = RANCH_POS_LIST[index][0];
    if(is_set_berfore == 0){
        glog_error("RANCH PTZ POS index[%d] was not set before\n", index);
        return -1;            
    }

    request_auto_move_ptz_stop();

    unsigned char send_data[32] = {0x96,0x00,0x01,0x01,0x0F};                     //LJH, 0x0101 command require response, data length is 15
    //2. set  move postion
    int move_speed = ptz_move_speed;
    memcpy(&send_data[5], &RANCH_POS_LIST[index][1], 10);
    move_speed = RANCH_PTZ_SPEED;

    //3. speed 설정 
    send_data[15] = send_data[16] = send_data[17] = send_data[18] = move_speed;   //LJH, [15] is Pan Speed, [16] is Tilt Speed, [17] is Zoom Speed, [18] is Focus Speed
    send_data[19] = 0;
    send_data[20] = get_checksum(send_data, 20);
    glog_trace("ranch_ptz_move index[%d] move_speed[%d]\n", index, move_speed);

    unsigned char read_data[8];  
    int result = read_cmd_timeout(send_data, 21, read_data, 7, 1);
    if(result == -1){
      glog_error("fail read move_ptz_pos\n");
      return result;
    }

    return 0;
}

#endif


int set_ptz_pos(int id, unsigned char* read_data, int auto_ptz_mode)
{
     if(!is_open_serial()){
        glog_error("serial device not opened before \n");
        return -1;
    }
    request_auto_move_ptz_stop();
    unsigned char cmd_data[7] = {0x96,0x00,0x06,0x01,0x01,0x01,0x9F};           //LJH, command is 0x0106 = Get All Position
    int result = read_cmd_timeout(cmd_data, 7, read_data, 17, 1);               //LJH, response data length is 11, whole response size is 17
    if(result == -1){
      glog_error("fail read set_ptz_pos \n");
      return result;
    }
    update_ptz_pos(id, &read_data[5], auto_ptz_mode);
    return 0;
}


int get_pt_status()
{
    unsigned char cmd_data[6] = {0x96,0x00,0x01,0x04,0x00,0x9B};                //LJH, command is 0x0401 = Get PT State
    unsigned char read_data[9];

    int result = read_cmd_timeout(cmd_data, 6, read_data, 8, 1);                //LJH, reponse date length is 2, whole response size is 8
    if(result == -1){
      glog_error("fail read get_pt_status \n");
      return result;
    }
    return read_data[6];
}

int move_ptz_pos(int index, int auto_ptz_move)
{
    if(!is_open_serial()){
        glog_error("serial device not opened before \n");
        return -1;
    }

    if(index >= MAX_PTZ_PRESET){
        glog_error("PTZ Preset [%d] exceed Max [%d]\n", index, MAX_PTZ_PRESET);
        return -1;
    }
    
    int is_set_berfore = auto_ptz_move? AUTO_PTZ_POS_LIST[index][0] : PTZ_POS_LIST[index][0];
    if(is_set_berfore == 0){
        glog_error("PTZ Preset mode[%d]  Not set before index [%d]\n", auto_ptz_move, index);
        return -1;            
    }

    //1. 현재의 포지션을 가져옴 
    //1. 현재 상태가 움직임 상태일 경우는 움직임을 멈춘다.
    if(!auto_ptz_move) {
        request_auto_move_ptz_stop();
        g_no_zoom = 0;          //LJH
    }

    unsigned char send_data[32] = {0x96,0x00,0x01,0x01,0x0F};     //LJH, 0x0101 command require response, data length is 15
    //2. set  move postion
    int move_speed = ptz_move_speed;
    if(auto_ptz_move) {
        memcpy(&send_data[5], &AUTO_PTZ_POS_LIST[index][1], 10);
        move_speed = auto_ptz_move_speed;
    } else {
        memcpy(&send_data[5], &PTZ_POS_LIST[index][1], 10);
    }

    //3. speed 설정 
    send_data[15] = send_data[16] = send_data[17] = send_data[18] = move_speed;   //LJH, [15] is Pan Speed, [16] is Tilt Speed, [17] is Zoom Speed, [18] is Focus Speed
    if (g_no_zoom) {        
      send_data[17] = 0;            //LJH, set zoom speed as 0
    }
    send_data[19] = 0;

    //4. CheckSum
    send_data[20] = get_checksum(send_data, 20);

    // glog_trace("move_ptz_pos auto_ptz_move[%d] index[%d]\n", auto_ptz_move, index);
    unsigned char read_data[8];  
    int result = read_cmd_timeout(send_data, 21, read_data, 7, 1);
    if(result == -1){
      glog_error("fail read move_ptz_pos \n");
      return result;
    }

    return 0;
}


#include <pthread.h>    
static pthread_t g_tid;
int is_work_auto_ptz()
{
    return AUTO_PTZ_MOVE_SEQ[0];
}


void request_auto_move_ptz_stop()
{
    //wait thread endup
    if(AUTO_PTZ_MOVE_SEQ[0]){
        AUTO_PTZ_MOVE_SEQ[0] = 0;         //LJH, make process_auto_move_ptz thread exit
        pthread_join(g_tid, NULL);        //LJH, blocking function
    }
}

gboolean is_ptz_motion_stopped()
{
  static int first = 1;

  if (first) {
    first = 0;
    pthread_mutex_init(&g_motion_mutex, NULL);
  }
  pthread_mutex_lock(&g_motion_mutex);  // Lock at the beginning

  if(is_open_serial() == 0){
    g_move_speed = 0;
    return TRUE;
  }

  int motion_status = get_pt_status();
  if ( (motion_status&0xF) == 0 ){
    g_move_speed = 0;
    // glog_trace("set g_move_speed = 0\n");
    pthread_mutex_unlock(&g_motion_mutex);  // Lock at the beginning
    return TRUE;
  }

  pthread_mutex_unlock(&g_motion_mutex);  // Lock at the beginning
  return FALSE;
}


unsigned int get_auto_move_zoom_val(int index)
{
  unsigned int val = 0;

  val = AUTO_PTZ_POS_LIST[index][4];
  val += AUTO_PTZ_POS_LIST[index][5] * 0xFF;

  return val;
}


int is_zoom_out(int prev, int cur)
{
  if(prev > cur)                  //LJH, smaller zoom value means far distance from the object
    return 1;
  return 0;
}


//LJH, zoom first is default operation of RGB camera when doing auto ptz
void *process_auto_move_ptz(void *arg)
{
    static int index = 0, prev_index = 0;                 //LJH, static int for continuous move 
    int sleep_100ms_cnt = AUTO_PTZ_MOVE_SEQ[1] * 10;
    int max_index = AUTO_PTZ_MOVE_SEQ[2];
    unsigned int cur_zoom_val = 0, prev_zoom_val = 0;
    int continue_tag = 0;

    glog_trace("start auto_move_ptz[%d], analysis stay time[%d]\n ",max_index, AUTO_PTZ_MOVE_SEQ[1]);

    while(AUTO_PTZ_MOVE_SEQ[0]) {                         //AUTO_PTZ_MOVE_SEQ[] 의 앞의 3개 값은 AI on/off, stay time, setting 된 AUTO_PTZ_POS_LIST 의 index list 의 count(=max index)
      if (continue_tag == 0) {
        prev_zoom_val = get_auto_move_zoom_val(prev_index);
        cur_zoom_val = get_auto_move_zoom_val(index);
        if (is_zoom_out(prev_zoom_val, cur_zoom_val))   
          g_no_zoom = 1;
        else 
          g_no_zoom = 0;
      }
      else {
        continue_tag = 0;
        g_no_zoom = 0;
      }

      move_ptz_pos(AUTO_PTZ_MOVE_SEQ[index+3], 1);

      if(g_setting.analysis_status) 
        set_process_analysis(0);                      //turn off AI

      for(int i = 0; i < (AUTO_PTZ_MOVE_SEQ[1] + 10) ; i++) {                  //LJH, max motion time
        if(AUTO_PTZ_MOVE_SEQ[0] == 0) {
          index = (index + 1)%max_index;              //LJH, increase index within max_index
          break;
        }
        usleep(ONE_SEC);                            
        if(is_ptz_motion_stopped()) {
          // glog_trace("PTZ stopped after %d sec\n", (i + 1));
          break;
        }
      }
      if(AUTO_PTZ_MOVE_SEQ[0] == 0) {                 //LJH, if thread exit was set then break
        index = (index + 1)%max_index;                //LJH, increase index within max_index
        break;
      }

      if(g_no_zoom) {                                 //LJH, do not analysis, analysis after zoom was done
        continue_tag = 1;
        continue;
      }
      else {
        continue_tag = 0;
      }

      g_preset_index = index;                         //LJH, 250204, set preset index
      if(g_setting.analysis_status) 
          set_process_analysis(1);                    //turn on AI

      //너무 길게 쉬어 줄 경우 기다려야 되는 문제가 있어서 100ms 단위로 쉬도록 한다.
      for(int i = 0 ; i < sleep_100ms_cnt ; i++){     //LJH, analysis lasting time 
          if(AUTO_PTZ_MOVE_SEQ[0] == 0)               //LJH, if thread exit was set then break
            break;
          usleep(ONE_TENTH_SEC);                      //LJH, 0.1 second sleep
      }

      for(int i = 0; i < 15; i++){                    //LJH, add analysis lasting time when event recording is on process
        if (g_notifier_running == 0)                  //LJH, if it is not recording
          break;
        sleep(1);
      }

      if(AUTO_PTZ_MOVE_SEQ[0] == 0)                   //LJH, if thread exit was set then break
        break;

      prev_index = index;
      index = (index + 1)%max_index;                  //LJH, increase index within max_index
    }
    g_no_zoom = 0;
    glog_trace("end auto_move_ptz\n");

    return 0;
}


int auto_move_ptz(const char* move_seq)
{    
    if(!is_open_serial()){
        glog_error("serial device not opened before \n");
        return -1;
    }

    unsigned char data[MAX_PTZ_PRESET + 8] = {0};
    int data_len = parse_string_to_hex(move_seq, data, MAX_PTZ_PRESET + 8);
    request_auto_move_ptz_stop();

    //데이터의 개수가 작을 경우는 error
    if(data_len < 4 || data_len > MAX_PTZ_PRESET + 2){
        glog_error("PTZ Auto Move Preset sequence not validate data_len [%d]\n", data_len);
        return -1;                        
    }
    
    if(data[data_len-2] != 0xFF){
        glog_error("PTZ Auto Move Preset sequence not validate, end mark not found \n");
        return -1;                                   
    }

    //check ptz 
    for(int i = 0 ; i < data_len-2 ; i++){
        if(data[i] >= MAX_PTZ_PRESET){
            glog_error("PTZ Auto Move Preset sequence not validate %d, => %d \n", i, data[i]);
            return -1;
        }

        if(AUTO_PTZ_POS_LIST[data[i]][0] == 0){
            glog_error("PTZ Auto Move Preset is not set: index value ==(data[%d]==%d)\n", i, data[i]);
            return -1;
        }
    }
    
    AUTO_PTZ_MOVE_SEQ[0] = 1;
    AUTO_PTZ_MOVE_SEQ[1] = data[data_len-1];
    AUTO_PTZ_MOVE_SEQ[2] = data_len-2;
    for(int i = 0 ; i < data_len-2 ; i++){
        AUTO_PTZ_MOVE_SEQ[3+i] = data[i];
    }

    glog_trace("start auto_move_ptz[%s], data len:%d [ ", move_seq, data_len-2);
    for(int i = 0 ; i < data_len + 1 ; i++){
       glog_plain("%x ", AUTO_PTZ_MOVE_SEQ[i]);
    }
    glog_plain("]\n");

    //start auto ptz mode
    pthread_create(&g_tid, NULL, process_auto_move_ptz, NULL);
    return 0;
}


gboolean send_ptz_move_cmd(int direction, int ptz_speed) 
{
  if(is_open_serial() == 0) {
   	glog_error("PTZ serial is not open\n");
    ptz_err_code = PTZ_NORMAL; 
    stop_retry_count = 0;      
    return FALSE;
  }

  unsigned char read_data[7] = {0}, data[11];
  char direction_str[][12] = {"left", "right", "top", "bottom", "zoom in", "zoom out"};
	int len = 0, size = sizeof(direction_str)/sizeof(direction_str[0]);

  if (size > direction && direction >= 0) {
    if (ptz_speed > 0) {
	    glog_trace("Send PTZ UART command(Direction:%s,Speed:%d)\n", direction_str[direction], ptz_speed);
    }
    else {
      glog_trace("Send PTZ UART stop command\n");
    }
  }
  else {
    glog_error("PTZ direction(=%d) is out of range\n", direction);
    return FALSE;
  }

	memset(data, 0x00, sizeof(data));

	data[len++] = 0x96;           //Sync
	data[len++] = 0x00;           //Addr
	data[len++] = 0x00;           //CmdH

  if (ptz_speed > 0)
	  data[len++] = 0x41;         //CmdL: Does not require response
   else 
    data[len++] = 0x01;         //CmdL: Require response

  data[len++] = 0x05;           //Data length

  if (ptz_speed > 0) {
    if (direction == 0) {       //Left 
      data[len++] = 0x40;
    } 
    else if(direction == 1) {  //Right
      data[len++] = 0x80;
    } 
    else if(direction == 2) {  //Top
      data[len++] = 0x10;
      data[len++] = 0x00;
    } 
    else if(direction == 3) {  //Bottom
      data[len++] = 0x20;
      data[len++] = 0x00;
    } 
    else if(direction == 4) {  //Zoom in
      data[len++] = 0x04;
      data[len++] = 0x00;
      data[len++] = 0x00;
    } 
    else if(direction == 5) {  //Zoom out
      data[len++] = 0x08;
      data[len++] = 0x00;
      data[len++] = 0x00;
    }
    data[len] = ptz_speed;      //PTZ speed
  }

  data[10] = get_checksum(data, 10);
  len = 11;

  if (ptz_speed > 0) {                                                          //if data is for move, if speed is not 0
    write_serial(data, len);
    ptz_err_code = PTZ_NORMAL;
    stop_retry_count = 0;
  }
  else {                          //if ptz_speed is 0 then it is stop command
    int iRet = read_cmd_timeout(data, len, read_data, 7, 1);                  
    if (iRet < 0) {
      ptz_err_code = PTZ_STOP_FAILED;
      glog_error("Reading serial timed out, PTZ stop failed\n");
    }
    else {
      glog_trace("Received UART response : ");
      if (iRet <= sizeof(read_data))
        print_serial_data(read_data, iRet);

//#define TEST_CODE
#ifdef TEST_CODE
      read_data[5] = 0x01;       
      glog_trace("TEST code is applied\n");
#endif

      if(read_data[4] == 0x01 && read_data[5] == 0x00) {            //read_data[4] is data length, read_data[5] is error code
        ptz_err_code = PTZ_NORMAL;       
        stop_retry_count = 0;
      }
      else {                                                        
        ptz_err_code = PTZ_STOP_FAILED;
        glog_error("Error code is %02X\n", read_data[5]);
      }
    }
  }
  g_move_speed = ptz_speed;
  // glog_trace("set g_move_speed = %d\n", g_move_speed);

	return TRUE;
}


static guint timer_id = 0;
static gboolean timer_stop_ptz_callback(gpointer user_data) 
{
  send_ptz_move_cmd(0,0);
  timer_id = 0;

  return G_SOURCE_REMOVE;         //LJH, this removes source id
}


gboolean move_and_stop_ptz(int direction, int ptz_speed, int ptz_delay)
{
	if (send_ptz_move_cmd(direction, ptz_speed) == TRUE) {
    if (timer_id > 0) { 
      g_source_remove(timer_id);
      timer_id = 0;
    }
    timer_id = g_timeout_add((guint)ptz_delay, timer_stop_ptz_callback, NULL);  
  }
  else {
    return FALSE;
  }

	return TRUE;
}


void send_ptz_move_serial_data(const char *s)
{
  int count = 0;
  int direction = 0, ptz_delay = 0, ptz_speed = 0;
  glog_trace ("Received PTZ command from server: %s\n", s);
  char str[100];
  strcpy(str, s);

  char *ptr = strtok((char*)str, ",");      
  while (ptr != NULL) {
    if (count == 0)
      direction = atoi(ptr);
    else if (count == 1)
      ptz_delay = atoi(ptr);
    else if (count == 2)
      ptz_speed = atoi(ptr);
    count++;
    ptr = strtok(NULL, ",");     
  }
  glog_trace("direction:%d,delay:%d,speed:%d\n", direction, ptz_delay, ptz_speed);
  move_and_stop_ptz(direction, ptz_speed, ptz_delay);
}
