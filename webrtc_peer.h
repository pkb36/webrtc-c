#ifndef __WEBRTC_PEER_H__
#define __WEBRTC_PEER_H__

#include <gst/gst.h>
#include "socket_comm.h"

gboolean  init_webrtc_peer(int max_peer_cnt, int device_cnt, int stream_base_port, char *codec_name, int comm_socket_port);
void      free_webrtc_peer(gboolean bFinal);

gboolean add_peer_to_pipeline (const gchar * peer_id, const gchar * channel);
void remove_peer_from_pipeline (const gchar * peer_id);

gboolean handle_peer_message (const gchar * peer_id, const gchar * msg);

gboolean start_process_rec();
void stop_process_rec();


#endif	// __WEBRTC_PEER_H__
