#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include "gstream_main.h"
#include "webrtc_peer.h"
#include "config.h"
#include "process_cmd.h"

extern WebRTCConfig g_config;

typedef struct 
{
  gchar*         peer_id;
  SOCKETINFO*    socket;
  pid_t          child_pid; 
}PeerInfo;

static int g_MaxPeerCnt = 0;
static PeerInfo* g_PeerInfos = NULL;
static int g_device_cnt, g_stream_base_port, g_comm_socket_port;
static char g_codec_name[16]; 

int   find_peer_index(const gchar * peer_id)
{
  int peer_idx = -1;
  for(int i = 0 ; i < g_MaxPeerCnt ; i++){
    if(g_PeerInfos[i].peer_id == 0) continue;

    if(strcmp(g_PeerInfos[i].peer_id, peer_id) ==0){
      peer_idx = i;
      break; 
    }
  }
  return peer_idx;
}


void  notify_webrtc_instance(char *data , int len, void* arg)
{
  //PeerInfo *peer_info = (PeerInfo *) arg;
  //gst_print("notify_webrtc_instance [%d][%s] \n", peer_info->peer_id, data);
  send_msg_server(data);
}


gboolean init_webrtc_peer(int max_peer_cnt, int device_cnt, int stream_base_port, char *codec_name, int comm_socket_port)
{
  g_MaxPeerCnt  = max_peer_cnt;
  g_device_cnt  = device_cnt; 
  g_stream_base_port = stream_base_port;
  g_comm_socket_port = comm_socket_port;
  strcpy(g_codec_name, codec_name);
  
  g_PeerInfos   = (PeerInfo*)calloc(max_peer_cnt, sizeof(PeerInfo));
  for(int i = 0 ; i < g_MaxPeerCnt ; i++){
    g_PeerInfos[i].peer_id = NULL;
    g_PeerInfos[i].socket = init_socket_comm_server(g_comm_socket_port + i);
    g_PeerInfos[i].socket->call_fun = notify_webrtc_instance;
    g_PeerInfos[i].socket->data = (PeerInfo*)&g_PeerInfos[i];
    g_PeerInfos[i].socket->connect = 0;
  }
  return TRUE;
}


void  free_webrtc_peer(gboolean bFinal)
{
  if(g_PeerInfos){
    for(int i = 0 ; i < g_MaxPeerCnt ; i++){
      if(g_PeerInfos[i].peer_id != NULL){
        remove_peer_from_pipeline(g_PeerInfos[i].peer_id);
      }
      if(bFinal)
        close_socket_comm(g_PeerInfos[i].socket);
    }
  }

  if(bFinal){
    g_MaxPeerCnt  = 0;
  }  
}


gboolean handle_peer_message (const gchar * peer_id, const gchar * msg)
{
  //1. find webrtc sender 
  int peer_idx = find_peer_index(peer_id);
  if(peer_idx == -1){
    glog_error("handle_peer_message can not find peer [%s]\n", peer_id);
    return FALSE;    
  }

  send_data_socket_comm(g_PeerInfos[peer_idx].socket, msg, strlen(msg), 0);
  return TRUE;
}


void remove_peer_from_pipeline (const gchar * peer_id)
{
  int peer_idx = find_peer_index(peer_id);
  if(peer_idx == -1){
    glog_error("remove_peer_from_pipeline can not find peer [%s]\n", peer_id);
    return;    
  }

  //send close message
  glog_trace("send endup peer_idx [%d]:  [%s] \n", peer_idx, g_PeerInfos[peer_idx].peer_id);

  int status, endpid;
  char msg[32] = "ENDUP";
  send_data_socket_comm(g_PeerInfos[peer_idx].socket, msg, strlen(msg), 0);

  endpid=waitpid(g_PeerInfos[peer_idx].child_pid,&status,0);

  free(g_PeerInfos[peer_idx].peer_id);
  g_PeerInfos[peer_idx].peer_id = 0;
  g_PeerInfos[peer_idx].child_pid = 0;
  g_PeerInfos[peer_idx].socket->connect = 0;

  glog_trace("remove_peer_from_pipeline peer [%d]:  %d\n", endpid, status);
}

//LJH: gstream_main 에서 webrtc_sender 를 호출
gboolean add_peer_to_pipeline (const gchar * peer_id, const gchar * channel)    //LJH, 사용자의 접속에 따라 반복적으로 호출됨.
{
  //check exist peer 
  int peer_idx = find_peer_index(peer_id);
  if(peer_idx != -1){
    glog_error("add_peer_to_pipeline exist peer_idex [%s]\n", peer_id);
    return FALSE;    
  }

  for(int i = 0 ; i < g_MaxPeerCnt ; i++){
    if(g_PeerInfos[i].peer_id == 0){
      peer_idx = i;
      break; 
    }
  }
  if(peer_idx == -1){
    glog_error("add_peer_to_pipeline can not find empty peer_idx [%s]\n", peer_id);
    return FALSE;    
  }
  glog_trace("find peer idx try add[%d] [%s]\n", peer_idx, peer_id);

  int index = 0;
  if(strcmp(channel,"RGB") == 0){
    index = 0;
  } else if(strcmp(channel,"Thermal") == 0){
    index = 1;
  } else if(strcmp(channel,"RGB2") == 0){
    index = 100;
  } else if(strcmp(channel,"Thermal2") == 0){
    index = 101;
  } else if(strcmp(channel,"RGB3") == 0){
    index = 200;
  } else if(strcmp(channel,"Thermal3") == 0){
    index = 201;
  } else if(strcmp(channel,"RGB4") == 0){
    index = 300;
  } else if(strcmp(channel,"Thermal4") == 0){
    index = 301;
  } else {
    glog_error("add_peer_to_pipeline not defined channel.. [%s]\n", channel);
    return FALSE;
  } 

  int   stream_base_port = g_stream_base_port + peer_idx* g_device_cnt + index;
  int   comm_socket_port = g_comm_socket_port+peer_idx;
  g_PeerInfos[peer_idx].peer_id = g_strdup(peer_id);
  
  int pid = fork();                   //LJH, 사용자의 접속에 따라 fork 가 계속 일어남.
  if(pid == 0){
    //./webrtc_sender --stream_cnt=1 --stream_base_port=5000 --comm_port=6000 --peer-id="test"C
    char *programName = "./webrtc_sender";
    // char *programName = "./webrtc_sender_go";
    char strtemp[4][64];
    char peer_id_arg[256];
    snprintf(strtemp[0], 64, "--stream_cnt=%d", 1);       //LJH, 1개씩 해서 필요에 따라 여러번 호출될 수 있음.
    snprintf(strtemp[1], 64, "--stream_base_port=%d", stream_base_port); 
    snprintf(strtemp[2], 64, "--comm_socket_port=%d", comm_socket_port); 
    snprintf(strtemp[3], 64, "--codec_name=%s", g_codec_name); 
    snprintf(peer_id_arg, 256, "--peer_id=%s", peer_id); 
    char *args[]={programName,strtemp[0], strtemp[1] ,strtemp[2], strtemp[3],peer_id_arg, NULL};
    execvp(programName, args);

    //printf("Start 1 ...%d \n", pid); 
  } else if(pid > 0){
    g_PeerInfos[peer_idx].child_pid = pid;
    glog_trace("Wait for strat clinet peer_idx[%d] to pid[%d] socket [%d] \n", peer_idx, pid, g_PeerInfos[peer_idx].socket->clientaddr); 

    //wait start webrtc sender ... 
    int i = 0;
    for (i = 0; i < 5; i++){
      sleep(1);
      if(g_PeerInfos[peer_idx].socket->connect == 0){
        glog_trace("Wait Client count [%d] to peer_idx[%d] to pid[%d] \n", i, peer_idx, pid); 
      } else {
        break;
      }
    }

    if(i == 5){
      glog_error("Wait Client Fail peer_idx[%d] to pid[%d] \n", i, peer_idx, pid); 
    }
  }

  return TRUE;
}


static pid_t g_record_pid = 0;
gboolean start_process_rec() 
{
  return TRUE;
}



void stop_process_rec()
{
  if(g_record_pid != 0){
     if (kill(g_record_pid, SIGTERM) == 0) {
        glog_trace("success stop record process \n");
    } else {
        glog_trace("fail stop record process \n");
    }
      g_record_pid = 0;
  }
}
