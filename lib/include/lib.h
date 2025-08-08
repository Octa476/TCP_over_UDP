
#pragma once

#include <cstdint>
#include "utils.h"
#include <arpa/inet.h>

/* Maximum segment size, change as you see fit */
#define MAX_DATA_SIZE 512
#define MAX_SEGMENT_SIZE (MAX_DATA_SIZE + sizeof(poli_tcp_data_hdr))

#define MAX_CONNECTIONS 32

typedef struct buffer_ring buffer_ring;
typedef struct segment_data segment_data;
#define EMPTY 0
#define FULL_SEND_DATA 1
#define FULL_NO_ACK 2
#define FULL 1
#define CONFIRMED 3

// Struct used to store the payload of a segment.
struct segment_data {
    char *buf;
    int len;    
};

// Struct used to implement the sliding window of the
// Selective Repeat algorithm.
struct buffer_ring {
    char *buf;
    int len;
    int seq;
    int status;
    struct buffer_ring *next;
    struct buffer_ring *prev;
};


/* Protocol control block. Used track different parameters about a connection. 
 * Will need to be extenden to solve the homework with other parameters such as
 * last_ack or status depending on how you implement your protocol. */
struct connection {
    /* common window for both the sender and receiver. */
    /* list window: A window representation */
    int sockfd; /* socket used for this connection */
    int conn_id; /* connection identifier */
    struct sockaddr_in servaddr; /* used to identify the destination */
    pthread_mutex_t con_lock; /* Used for syncronization with the handler thread and read/send calls.*/

    int max_window_seq; /* Used to store the max number of packets that can be inflight, since we can
                           have many more packets in our window */
    int seq;
    int max_seq;

    int max_buffer_len;
    buffer_ring *ring_buffer;
    buffer_ring *first_empty_cell;
    int free_buffer_cells;

};

/* ########## API that we expose to the application ########### */

/* Equivalent of listen. Ran by the server to waits for a connection from a
 * client. Returns a connection id. Blocking untill it receives a connection
 * request */
int wait4connect(uint32_t ip, uint16_t port);
/* Equivalent of connect. Used by the client to connect to a server. */
int setup_connection(uint32_t ip, uint16_t port);
/* Equivalent to recv. Blocking if there is no data to be written in buffer */
int recv_data(int connectionid, char *buffer, int len);
/* Equivalent to send. Used by the client to send a stream of bytes as segments */
int send_data(int conn_id, char *buffer, int len);
/* Used to initialize your protocol on the receiver side. */
void init_receiver(int recv_buffer_bytes);
/* Used to initialize your protocol on the sender side */
void init_sender(int speed, int delay);

/* ######### Internal API used by sender and receiver ########### */
int recv_message_or_timeout(char *buff, size_t len, int *conn_id);

/* Functions that manipulate the ring_buffer */
buffer_ring *create_buf_cell();
buffer_ring *create_buffer(int len);
void destroy_buffer(buffer_ring *head);

/* Functions that manipulate the segment queue */
int add_seg_to_queue(struct connection *con, char *buf, int len);
int recv_seg_to_queue(struct connection *con);

/* Functions that manipulate the interaction between 
the segment queue and buffer_ring */
int add_seg_buffer(struct connection *con);
int extract_data_buffer(struct connection *con, char *buf, int len);

/* Functions used to manipulate the segments of the ring_buffer */
int send_seg_buffer(struct connection *con, int type);
int confirm_seg_buffer(struct connection *con, char * buf, int len);
int receive_data_buffer(struct connection *con, char *buf, int len);