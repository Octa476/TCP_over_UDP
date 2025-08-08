#include <pthread.h>
#include <cstdlib>
#include <map>
#include <cstdint>
#include "lib.h"
#include "utils.h"
#include "protocol.h"
#include <cassert>
#include <poll.h>
#include <sys/timerfd.h>
#include <string.h>

#include <queue>

using namespace std;

std::map<int, struct connection *> cons;

struct pollfd data_fds[MAX_CONNECTIONS];
/* Used for timers per connection */
struct pollfd timer_fds[MAX_CONNECTIONS];
int fdmax = 0;

int max_window_size;
std::queue<segment_data *> segment_queues[MAX_CONNECTIONS];

int send_data(int conn_id, char *buffer, int len)
{
    int size = 0;

    pthread_mutex_lock(&cons[conn_id]->con_lock);

    /* We will write code here as to not have sync problems with sender_handler */
    
    // Try to save the received data from the buffer into segment queue.
    int rc = 0;
    int seg_len = 0;
    while (len > 0) {
        // Save data segments of size <= MAX_DATA_SIZE. 
        if (len > MAX_DATA_SIZE)
            seg_len = MAX_DATA_SIZE;
        else
            seg_len = len;
        
        // Add the segment to the segment queue.
        rc = add_seg_to_queue(cons[conn_id], buffer + size, seg_len);

        // Check if the segment was added or not.
        if (rc) {
            len -= seg_len;
            size += seg_len;
        } else
            break;
    }

    pthread_mutex_unlock(&cons[conn_id]->con_lock);
    if (size)
        return size;
    
    // No data was saved.
    return -1;
}

void *sender_handler(void *arg)
{
    int res = 0;
    char buf[MAX_SEGMENT_SIZE];

    while (1) {

        if (cons.size() == 0) {
            continue;
        }
        int conn_id = -1;
        do {
            res = recv_message_or_timeout(buf, MAX_SEGMENT_SIZE, &conn_id);
        } while(res == -14);

        pthread_mutex_lock(&cons[conn_id]->con_lock);

        /* Handle segment received from the receiver. We use this between locks
        as to not have synchronization issues with the send_data calls which are
        on the main thread */

        // On timeout, send all the segments that have the status FULL_NO_ACK and FULL_SEND_DATA.
        if (res == -1) {

            // Add all available segments from the queue to the ring_buffer.
            int rc = 1;
            while (rc == 1)
                rc = add_seg_buffer(cons[conn_id]);

            // Send the segments.
            int num = send_seg_buffer(cons[conn_id], FULL_NO_ACK);
        } else {
            // A control segment was received, confirm the the segment.
            confirm_seg_buffer(cons[conn_id], buf, res);

            // Add any available segments form queue to the ring_buffer.
            int rc = 1;
            while (rc == 1)
                rc = add_seg_buffer(cons[conn_id]);
            
            // Send the segments with status FULL_SEND_DATA only.
            int num = send_seg_buffer(cons[conn_id], FULL_SEND_DATA);
        }

        pthread_mutex_unlock(&cons[conn_id]->con_lock);
    }
}

int setup_connection(uint32_t ip, uint16_t port)
{
    /* Implement the sender part of the Three Way Handshake. Blocks
    until the connection is established */

    struct connection *con = (struct connection *)malloc(sizeof(struct connection));
    int conn_id = 0;
    con->sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    // This can be used to set a timer on a socket 
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 20000;
    if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
        perror("Error");
    }

    /* We will send the SYN on 8031. Then we will receive a SYN-ACK with the connection
     * port. We can use con->sockfd for both cases, but we will need to update server_addr
     * with the port received via SYN-ACK */
    
    // Prepare some structs for data sending.
    struct sockaddr_in serv_addr;
    socklen_t slen = sizeof(serv_addr);

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = port;
    serv_addr.sin_addr.s_addr = ip;

    // Initialize the first seq number.
    con->seq = 0;
    con->max_window_seq = max_window_size;

    int rc = 0;
    while (1) {
        // Send SYN (send seq number).
        uint32_t seq = 0;
        rc = sendto(con->sockfd, &seq, sizeof(seq), 0,
                    (struct sockaddr *)&serv_addr, slen);
        printf("sendto failde me!\n");
        if (rc < 0) {
            fprintf(stderr, "sendto failed!\n");
        }

        // Receive SYN-ACK (receive seq and ack numbers).
        uint32_t seq_ack_window[3];
        rc = recvfrom(con->sockfd, seq_ack_window, sizeof(seq_ack_window), 0,
                        (struct sockaddr *)&serv_addr, &slen);
        if (rc < 0) {
            fprintf(stderr, "recvfrom failed, SYN-ACK was not received, sending SYN again!\n");
            continue;
        }

        if (seq + 1 != seq_ack_window[1]) {
            fprintf(stderr, "ack was corupted, sending SYN again!\n");
            continue;
        }
        
        // At this point serv_addr contains the port of the data socket.
        // Save this port in the con struct.
        memset(&con->servaddr, 0, sizeof(con->servaddr));
        con->servaddr.sin_family = AF_INET;
        con->servaddr.sin_port = serv_addr.sin_port;
        con->servaddr.sin_addr.s_addr = ip;
        con->max_buffer_len = seq_ack_window[2];

        while (1) {
            // Sending 10 ACKS (send ack number).
            uint32_t ack = seq_ack_window[0] + 1;
            for (int i = 0; i < 10; i++) {
                rc = sendto(con->sockfd, &ack, sizeof(ack), 0,
                            (struct sockaddr *)&serv_addr, slen);
                if (rc < 0) {
                    fprintf(stderr, "sendto failed!\n");
                }
            }

            tv.tv_sec = 0;
            tv.tv_usec = 40000;
            if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
                perror("Error");
            }

            rc = recvfrom(con->sockfd, seq_ack_window, sizeof(seq_ack_window), 0,
                            (struct sockaddr *)&serv_addr, &slen);
            if (rc < 0) {
                fprintf(stdout, "SYN-ACK was not received again, so the server received the ACK!\n");
                break;
            } else {
                // Send ACK again.
                fprintf(stdout, "ACK was not received by server, sending ACK again!\n");
                continue;
            }
        }

        // Delete the timer of the socket.
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
            perror("Error");
        }

        break;
    }

    // Create the ring_buffer.
    con->ring_buffer = create_buffer(con->max_buffer_len);
    con->free_buffer_cells = con->max_buffer_len;
    con->first_empty_cell = con->ring_buffer;
    con->max_seq = con->max_buffer_len - 1;

    /* Since we can have multiple connection, we want to know if data is available
       on the socket used by a given connection. We use POLL for this */
  
    
    /* This creates a timer and sets it to trigger every 1 sec. We use this
       to know if a timeout has happend on our connection */
    timer_fds[fdmax].fd = timerfd_create(CLOCK_REALTIME,  0);    
    timer_fds[fdmax].events = POLLIN;    
    struct itimerspec spec;     
    spec.it_value.tv_sec = 0;    
    spec.it_value.tv_nsec = 10000000;    
    spec.it_interval.tv_sec = 0;    
    spec.it_interval.tv_nsec = 10000000;    
    timerfd_settime(timer_fds[fdmax].fd, 0, &spec, NULL);    


    pthread_mutex_init(&con->con_lock, NULL);
    cons.insert({conn_id, con});

    DEBUG_PRINT("Connection established!");
    data_fds[fdmax].fd = con->sockfd;    
    data_fds[fdmax].events = POLLIN;
    fdmax++;
    return 0;
}

void init_sender(int speed, int delay)
{
    pthread_t thread1;
    int ret;

    max_window_size = speed * delay * (int)1e3 / 8 / MAX_SEGMENT_SIZE;

    /* Create a thread that will*/
    ret = pthread_create( &thread1, NULL, sender_handler, NULL);
    assert(ret == 0);
}
