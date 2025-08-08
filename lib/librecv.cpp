#include <pthread.h>
#include <cstdlib>
#include <map>
#include <cstdint>
#include "lib.h"
#include "utils.h"
#include "protocol.h"
#include <poll.h>
#include <cassert>
#include <sys/timerfd.h>
#include <string.h>

#include <queue>

using namespace std;

std::map<int, struct connection *> cons;

struct pollfd data_fds[MAX_CONNECTIONS];
/* Used for timers per connection */
struct pollfd timer_fds[MAX_CONNECTIONS];
int fdmax = 0;

int connectionfd;
int max_buffer_len;
int recv_buffer_bytes_recv;

std::queue<segment_data *> segment_queues[MAX_CONNECTIONS];

int recv_data(int conn_id, char *buffer, int len)
{
    int size = 0;
    while (size == 0) {
        pthread_mutex_lock(&cons[conn_id]->con_lock);
        
        /* We will write code here as to not have sync problems with recv_handler */
        // Just extract some data already cooked by the handler.
        size = extract_data_buffer(cons[conn_id], buffer, len);

        pthread_mutex_unlock(&cons[conn_id]->con_lock);
    }

    return size;
}

void *receiver_handler(void *arg)
{

    char segment[MAX_SEGMENT_SIZE];
    int res;
    DEBUG_PRINT("Starting recviver handler\n");

    while (1) {

        int conn_id = -1;
        do {
            res = recv_message_or_timeout(segment, MAX_SEGMENT_SIZE, &conn_id);
        } while(res == -14);

        pthread_mutex_lock(&cons[conn_id]->con_lock);

        /* Handle segment received from the sender. We use this between locks
        as to not have synchronization issues with the recv_data calls which are
        on the main thread */

        if (res != -1) {
            // Just to be sure, reveive some data from the ring_buffer
            // to keep it clean for the link receive.
            recv_seg_to_queue(cons[conn_id]);
            // Receive some data from the link and save it into the ring_buffer.
            int rc = receive_data_buffer(cons[conn_id], segment, res);
            // Let's move some data from ring_buffer to segment queue again.
            recv_seg_to_queue(cons[conn_id]);
        } else {
            // On timeout, just retreive some data from the ring_buffer.
            recv_seg_to_queue(cons[conn_id]);
        }

        pthread_mutex_unlock(&cons[conn_id]->con_lock);
    }

    
}

int wait4connect(uint32_t ip, uint16_t port)
{
    /* TODO: Implement the Three Way Handshake on the receiver part. This blocks
     * until a connection is established. */

    struct connection *con = (struct connection *)malloc(sizeof(struct connection));
    int conn_id = fdmax;

    /* This can be used to set a timer on a socket, useful once we received a
     * SYN. You may want to disable by setting the time to 0 (tv_sec = 0,
     * tv_usec = 0)
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 100000;
    if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0) {
        perror("Error");
    } */

    struct sockaddr_in client_addr;
    socklen_t clen = sizeof(client_addr);

    int rc = 0;
    while (1) {
        /* Receive SYN on the connection socket. Create a new socket and bind it to
        * the chosen port. Send the data port number via SYN-ACK to the client */
        int seq = 0;
        rc = recvfrom(connectionfd, &seq, sizeof(seq), 0,
                    (struct sockaddr *)&client_addr, &clen);
        if (rc < 0) {
            fprintf(stderr, "recvfrom failed!\n");
        }

        // SYN was received.
        con->sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVBUF, &recv_buffer_bytes_recv, sizeof(recv_buffer_bytes_recv)) < 0) {
            perror("setsockopt SO_RCVBUF failed");
            exit(EXIT_FAILURE);
        }
        // Set a timer on the connectionfd socket as it needs to receive ack.
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 20000;
        if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0)
            perror("Error");

        // Send SYN-ACK.
        uint32_t seq_ack_window[3];
        seq_ack_window[0] = seq; // seq
        seq_ack_window[1] = seq + 1; // ack
        seq_ack_window[2] = max_buffer_len; // max_buffer_len

        while (1) {
            for (int i = 0; i < 10; i++) {
                rc = sendto(con->sockfd, &seq_ack_window, sizeof(seq_ack_window), 0,
                            (struct sockaddr *)&client_addr, clen);
                if (rc < 0) {
                    fprintf(stderr, "sendto failed!\n");
                }
            }

            uint32_t ack = 0;
            rc = recvfrom(con->sockfd, &ack, sizeof(ack), 0,
                            (struct sockaddr *)&client_addr, &clen);

            if (rc < 0) {
                // ACK was not received.
                // Send SYN-ACK again.
                fprintf(stderr, "ack was not received, send SYN-ACK again\n");
                continue;
            }

            printf("ack: %d\n", seq_ack_window[1]);
            printf("seq: %d\n", seq_ack_window[0]);
            if (ack == seq_ack_window[0] + 1) {
                // ACK was received successfully.
                // Data sending may begin.
                fprintf(stderr, "ack is good, data sending may begin!\n");
                break;
            } else {
                // ACK is wrong, send SYN-ACK again.
                fprintf(stderr, "ack was bad, send SYN-ACK again\n");
                continue;
            }
        }
        // ACK was received successfully. 
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        if (setsockopt(con->sockfd, SOL_SOCKET, SO_RCVTIMEO,&tv,sizeof(tv)) < 0)
            perror("Error");
        
        con->servaddr = client_addr;
        break;
    }

    con->max_buffer_len = max_buffer_len; 
    con->ring_buffer = create_buffer(con->max_buffer_len);
    con->free_buffer_cells = con->max_buffer_len;
    con->first_empty_cell = con->ring_buffer;

    /* Since we can have multiple connection, we want to know if data is available
       on the socket used by a given connection. We use POLL for this */   
    
    /* This creates a timer and sets it to trigger every 1 sec. We use this
       to know if a timeout has happend on a connection */
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

    con->conn_id = conn_id;
    data_fds[fdmax].fd = con->sockfd;    
    data_fds[fdmax].events = POLLIN; 
    fdmax++;
    return conn_id;
}

void init_receiver(int recv_buffer_bytes)
{
    pthread_t thread1;
    int ret;

    recv_buffer_bytes_recv = recv_buffer_bytes;
    max_buffer_len = recv_buffer_bytes / MAX_SEGMENT_SIZE - 5;
    /* TODO: Create the connection socket and bind it to 8032 */

    struct sockaddr_in servaddr;

    // Creating socket file descriptor
    if ((connectionfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET; // IPv4
    // 0.0.0.0, basically match any IP
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(8032);

    int rc = bind(connectionfd, (const struct sockaddr *)&servaddr, sizeof(servaddr));
    if (rc < 0)
        perror("bind failed!");

    ret = pthread_create( &thread1, NULL, receiver_handler, NULL);
    assert(ret == 0);
}
