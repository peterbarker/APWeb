#include "includes.h"
#include "rtsp_ipc.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>

const char SOCKET_PATH[80] = "/tmp/rtsp_server.sock";
// const char SOCKET_PATH[80] = "../rtsp_server";

const char* RTSP_MESSAGE_HEADER[] = {
    "GDP", "SDP"
};

int send_server_message(char* msg)
{
    struct sockaddr_un addr;
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket error");
        return -1;
    }
    fprintf(stderr, "Socket fd - %d\n", fd);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "Connect error");
        return -1;
    } else {
        write(fd, msg, strlen(msg));
        close(fd);
    }
    return 0;
}

void get_server_response(RTSP_MESSAGE_TYPE type, char* reply, char* args)
{
    struct sockaddr_un addr;
    int fd;

    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket error");
        reply[0] = '\0';
        return;
    }
    fprintf(stderr, "Socket fd - %d\n", fd);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCKET_PATH);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        reply[0] = '\0';
        fprintf(stderr, "Connect error");
    } else {
        switch(type) {
        case GET_DEVICE_PROPS:
            write(fd, "GDP", 4);
            break;
        default:
            fprintf(stderr, "Not implemented yet!");
        }

        char read_buffer[1000];
        read_buffer[0] = '\0';
        int bytes_read = recv(fd, read_buffer, sizeof(read_buffer), 0);
        printf("\nRead buffer - %s with bytes - %d \n", read_buffer, bytes_read);
        read_buffer[bytes_read] = '\0';
        if (bytes_read != -1) {
            strcpy(reply, read_buffer);
        }
    }
    close(fd);
}

RTSP_MESSAGE_TYPE get_message_type(char* header)
{
    for (int i = 0; i < RTSP_MESSAGE_TYPE_COUNT; i++) {
        if (!strcmp(header, RTSP_MESSAGE_HEADER[i])) {
            return i;
        }
    }
    return ERROR;
}

void process_server_response(char* reply, char* result)
{
    char* p = strtok(reply, "$");

    char* msg_header = strdup(p);
    RTSP_MESSAGE_TYPE message_type;
    message_type = get_message_type(msg_header);

    p = strtok(NULL, "$");

    switch (message_type) {
    case GET_DEVICE_PROPS:
        ;
        printf("Got JSON - %s", p);
        strcpy(result, p);
        break;
    default:
        fprintf(stderr, "Not implemented yet!");
    }
}

void get_interfaces_list(char* interface_list)
{
    struct ifaddrs *ifaddr, *ifa;
    // int s;
    char host[NI_MAXHOST];

    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }
    interface_list[0] = '\0';
    sprintf(interface_list, "{\"interfaces\": [");
    int i = 0;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) {
            continue;
        }
        getnameinfo(ifa->ifa_addr,sizeof(struct sockaddr_in), host, NI_MAXHOST,
                      NULL, 0, NI_NUMERICHOST);
        if (ifa->ifa_addr->sa_family==AF_INET) {
            fprintf(stderr, "%s\n", ifa->ifa_name);
            if (i == 0) {
                sprintf(interface_list, "%s\"%s\"", interface_list, ifa->ifa_name);    
            } else {
                sprintf(interface_list, "%s,\"%s\"", interface_list, ifa->ifa_name);
            }
            i++;
        }
        // if ((strcmp(ifa->ifa_name, interface.c_str()) == 0) && (ifa->ifa_addr->sa_family==AF_INET)) {
        //     if (s != 0) {
        //         printf("getnameinfo() failed: %s\n", gai_strerror(s));
        //     }
        //     printf("Interface : <%s>\n",ifa->ifa_name);
        //     printf("Address : <%s>\n", host);
        //     freeifaddrs(ifaddr);
        //     return string(host);
        // }
    }
    sprintf(interface_list, "%s]}", interface_list);
    fprintf(stderr, "IF list - %s\n", interface_list);

    freeifaddrs(ifaddr);
    // return string("127.0.0.1");
}