#ifndef RTSP_IPC_H
#define RTSP_IPC_H

static const unsigned int IPC_BUFFER_SIZE = 10000;

typedef enum RTSP_MESSAGE_TYPE {GET_DEVICE_PROPS, 
                                SET_DEVICE_PROPS,
                                ERROR,
                                RTSP_MESSAGE_TYPE_COUNT} 
                                RTSP_MESSAGE_TYPE;

void get_server_response(RTSP_MESSAGE_TYPE type, char* reply, char* args);
int send_server_message(char* msg);
void process_server_response(char* reply, char* result);
void get_interfaces_list(char* interface_list);
RTSP_MESSAGE_TYPE get_message_type(char* header);

#endif
