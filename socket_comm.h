#ifndef __SOCKET_COMM_H__
#define __SOCKET_COMM_H__

#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include "device_setting.h"
#include "curllib.h"
#include "config.h"

typedef struct 
{
  int 				    socketfd;
  unsigned short  port;
  pthread_t 		  tid;
  
  //call back func
  void *data;
  void (*call_fun)(char *ptr , int len, void* arg);

  struct sockaddr_in	clientaddr;
  int   connect;
}SOCKETINFO;

SOCKETINFO* init_socket_comm_server(int port);
SOCKETINFO* init_socket_comm_client(int port);

// All extern declarations moved to globals.h
// Include globals.h in your .c files instead

int   send_data_socket_comm(SOCKETINFO* socket, const char* data, int len, int is_self);
void  close_socket_comm(SOCKETINFO* socket);

#endif	// __SOCKET_COMM_H__
