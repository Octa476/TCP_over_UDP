#include <sys/socket.h>
#include <sys/types.h>
#include <sys/timerfd.h>
#include <netinet/ip.h> 
#include <stdio.h>
#include <cstdint>
#include "lib.h"
#include <vector>
#include <map>
#include <cstring>
#include <assert.h>
#include <unistd.h>
#include <cstdlib>
#include <pthread.h>
#include <sys/poll.h>
#include <fcntl.h>

#include <queue>

#include "protocol.h"

using namespace std;

/* We use extern to use the variables defined either in
   librecv or libsend */
extern std::map<int, struct connection *> cons;
extern struct pollfd data_fds[MAX_CONNECTIONS];
extern struct pollfd timer_fds[MAX_CONNECTIONS];
extern int fdmax;

extern std::queue<segment_data *> segment_queues[MAX_CONNECTIONS];

int recv_message_or_timeout(char *buff, size_t len, int *conn_id)
{
    int ret, i;

    /* We check if data is available on a socket or if a timer has expired */
    ret = poll(data_fds, fdmax, 0); 
    assert(ret >= 0);
    ret = poll(timer_fds, fdmax, 0); 
    assert(ret >= 0);

    for (i = 0; i < fdmax; i++) {
        /* Data available on a socket */
        if (data_fds[i].revents & POLLIN) {

            struct sockaddr_in servaddr;
            socklen_t slen = sizeof(struct sockaddr_in);

            int n = recvfrom(data_fds[i].fd, buff, len, MSG_WAITALL, (struct sockaddr *) &servaddr , &slen);
            int j = -1; 

            /* Find which connection this socket coresponds to */
            for (auto const& x : cons) {
                if(x.second->sockfd == data_fds[i].fd) {
                    j = x.first;
                    break;
                }
            }

            assert(j != -1);
            /* Write in the conn_id the connection of the socket*/
            *conn_id = j;
            return n;
        }
        /* A timer has expired on a connection */
        if (timer_fds[i].revents & POLLIN) {

            char dummybuf[8];
            read(timer_fds[i].fd, dummybuf, 8);

            /* Find which connection this coresponds to */
            int j = -1; 
            for (auto const& x : cons) {
                if(x.second->sockfd == data_fds[i].fd) {
                    j = x.first;
                    break;
                }
            }
            assert(j != -1);

            /* Write in the conn_id the connection of the socket*/
            *conn_id = j;
            return -1;
        }
    }

    /* If nothing happened, return -14. */
    return -14;
}

buffer_ring *create_buf_cell() {
    buffer_ring *cell = (buffer_ring *)malloc(sizeof(buffer_ring));
    if (!cell) {
        fprintf(stderr, "malloc failed!\n");
        exit(1);
    }

    cell->next = cell;
    cell->prev = cell;
    cell->status = EMPTY;

    cell->buf = (char *)malloc(MAX_SEGMENT_SIZE * sizeof(char));
    if (!cell->buf) {
        fprintf(stderr, "malloc failed!\n");
        exit(1);
    }

    return cell;
}

buffer_ring *create_buffer(int len) {
    if (len == 0)
        return NULL;
    
    buffer_ring *head = create_buf_cell();

    if (len == 1)
        return head;

    buffer_ring *ant = head;
    buffer_ring *curr = NULL;

    for (int i = 1; i < len; i++) {
        curr = create_buf_cell();
        ant->next = curr;
        curr->prev = ant;
        ant = curr;
    }

    curr->next = head;
    head->prev = curr;

    return head;
}

// Add a new segment to the segment queue of a connection.
int add_seg_to_queue(struct connection *con, char *buf, int len) {
    if (len > MAX_DATA_SIZE) {
        fprintf(stderr, "segment is too big!\n");
        exit(1);
    }

    segment_data *seg_data = (segment_data *)malloc(sizeof(segment_data));
    if (!seg_data) {
        fprintf(stderr, "malloc failed!\n");
        exit(1);
    }

    seg_data->buf = (char *)malloc(MAX_DATA_SIZE * sizeof(char));
    if (!seg_data->buf) {
        fprintf(stderr, "malloc failed!\n");
        exit(1);
    }

    memcpy(seg_data->buf, buf, len);
    seg_data->len = len;
    segment_queues[con->conn_id].push(seg_data);

    return 1;
}

// Take the segments from the segment queue and add them to the ring_buffer.
int add_seg_buffer(struct connection *con) {

    // Queue is empty.
    if (segment_queues[con->conn_id].empty())
        return 2;
    
    // An illegal action occured, there are more free cells than the total
    // number of cells.
    if (con->free_buffer_cells > con->max_buffer_len)
        fprintf(stderr, "free_buffer_cells: %d\n", con->free_buffer_cells);

    // If the ring buffer have at least a free cell, fill it.
    if ((con->free_buffer_cells) && (con->first_empty_cell->status == EMPTY)) {
        segment_data *seg_data = segment_queues[con->conn_id].front();
    
        char *buf = seg_data->buf;
        int len = seg_data->len;

        // An illegal action of desincronization occured, the sliding
        // window exceeded the len of the ring_buffer.
        if (con->max_seq > con->seq + con->max_buffer_len - 1)
            fprintf(stderr, "sooo strange!\n");

        int seq = con->seq;
        con->seq++;

        // Prepare the data segment to be saved into the ring_buffer.
        struct poli_tcp_data_hdr data_hdr;
        data_hdr.protocol_id = POLI_PROTOCOL_ID;
        data_hdr.conn_id = con->conn_id;
        data_hdr.seq_num = seq;
        data_hdr.type = DATA_TYPE;
        data_hdr.len = len;
        memcpy(con->first_empty_cell->buf, &data_hdr, sizeof(struct poli_tcp_data_hdr));

        memcpy(con->first_empty_cell->buf + sizeof(struct poli_tcp_data_hdr), buf, len);
        con->first_empty_cell->status = FULL_SEND_DATA;
        con->first_empty_cell->len = sizeof(struct poli_tcp_data_hdr) + len;
        con->first_empty_cell->seq = seq;
        
        con->first_empty_cell = con->first_empty_cell->next;
        con->free_buffer_cells--;

        // Purge the memory used by the queue.
        free(buf);
        segment_queues[con->conn_id].pop();
        return 1;
    }

    // Another illegal state has been achieved, the slinding
    // window exceeded the ring+buffer's length.
    if (con->max_seq + 1 < con->seq)
        fprintf(stderr, "illegal seq number in sender\n");

    // No empty cell was found.
    return 0;
}

// Send some segments based on the type of call.
int send_seg_buffer(struct connection *con, int type) {
    buffer_ring *curr = con->ring_buffer;
    buffer_ring *first_empty_cell = con->first_empty_cell;
    int indx = 0;

    int rc = 0;
    int slen = sizeof(struct sockaddr_in);
    int num = 0;

    // While we are checking a cell that is not empty, always send
    // the cell with FULL_SEND_DATA status and send the cells with
    // status FULL_NO_ACK just when ascked(this action is triggered by a timer).
    while ((!indx) || (curr != first_empty_cell)) {
        if (((curr->status == type) && (type == FULL_SEND_DATA)) || 
        ((type == FULL_NO_ACK) && ((curr->status == FULL_NO_ACK) || (curr->status == FULL_SEND_DATA)))) {
            curr->status = FULL_NO_ACK;
            num++;
            rc = sendto(con->sockfd, curr->buf, curr->len, 0,
                        (struct sockaddr *)&con->servaddr, slen);
            if (rc < 0)
                fprintf(stderr, "sendto failed in send_handler!\n");
        }
        curr = curr->next;
        indx++;
    }
    return num;
}

// This function confirms a cell from the ring_buffer. 
int confirm_seg_buffer(struct connection *con, char *buf, int len) {
    // Checking the size of the ctrl segment.
    if (len != sizeof(struct poli_tcp_ctrl_hdr)) {
        fprintf(stderr, "the transmision of ack segment is bad!\n");
        return 0;
    }

    struct poli_tcp_ctrl_hdr *ctrl_segment = (struct poli_tcp_ctrl_hdr *)buf;

    // Checking the type of the ctrl segment.
    if (ctrl_segment->type != ACK_TYPE) {
        fprintf(stderr, "the type of ack segment is bad!\n");
        return 0;
    }

    buffer_ring *curr = con->ring_buffer;
    buffer_ring *first_empty_cell = con->first_empty_cell;
    int indx = 0;

    // Search for the cell that needs to be confirmed.
    while ((!indx) || (curr != first_empty_cell)) {
        if (curr->seq == ctrl_segment->ack_num) {
            curr->status = CONFIRMED;

            // If the CONFIRMED segment is the head of the ring_buffer
            // move the window to the first UNCONFIRMED segment.
            if (curr == con->ring_buffer) {
                while (con->ring_buffer->status == CONFIRMED) {
                    con->ring_buffer->status = EMPTY;
                    con->free_buffer_cells++;
                    con->max_seq++;
                    con->ring_buffer = con->ring_buffer->next;
                }
            }

            break;
        }
        curr = curr->next;
        indx++;
    }

    return 0;

}

// This function is used to receive data segments from the link and save
// them into the ring_buffer.
int receive_data_buffer(struct connection *con, char *buf, int len) {
    int ret = 0;
    struct poli_tcp_data_hdr *data_hdr = (struct poli_tcp_data_hdr *)buf;

    if (data_hdr->type != DATA_TYPE) {
        fprintf(stderr, "the type of the data was wrong!\n");
        return 0;
    }

    buffer_ring *curr = con->ring_buffer;
    int indx = con->seq;

    /* This was meant to be a warning but theoretically it can't happen */

    // Here client was faster than the server, sending a segment
    // with an illegal ack(it is a segment outside the server's window).
    // This happened because after the server sent the last ack, the client
    // received it, and moved the window by one cell, sending the new data
    // segment to the server, but the server didn't use the segment of which
    // ack was sent, so its window didn't move => illegal segment seq.

    // It isn't that bad I guess as the server will use the last segment
    // and the client will send the data segment until the ack will be received,
    // so this misanderstanding will just put one or two more segments on the line.
    if (data_hdr->seq_num - indx >= con->max_buffer_len) {
        fprintf(stderr, "illegal seq number: %d\n", data_hdr->seq_num);
        fprintf(stderr, "diff: %d\n", data_hdr->seq_num - indx);
        return 2;
    }

    // Send ack duplicate.
    if (data_hdr->seq_num < con->seq) {
        struct poli_tcp_ctrl_hdr ctrl_segment;
        ctrl_segment.protocol_id = POLI_PROTOCOL_ID;
        ctrl_segment.conn_id = con->conn_id;
        ctrl_segment.ack_num = data_hdr->seq_num;
        ctrl_segment.type = ACK_TYPE;

        // Send ack.
        int rc = 0;
        int clen = sizeof(struct sockaddr_in);
        rc = sendto(con->sockfd, &ctrl_segment, sizeof(ctrl_segment), 0,
            (struct sockaddr *)&con->servaddr, clen);
        if (rc < 0)
            fprintf(stderr, "sendto failed in send_handler!\n");

        return 0;
    }

    // Find the cell that needs to be filled with the received segment.
    while (indx != data_hdr->seq_num) {
        indx++;
        curr = curr->next;
    }

    // The cell is empty.
    if (curr->status == EMPTY) {
        int data_len = len - sizeof(struct poli_tcp_data_hdr);
        curr->len = data_len;
        curr->seq = data_hdr->seq_num;
        curr->status = FULL;
        memcpy(curr->buf, buf + sizeof(struct poli_tcp_data_hdr), data_len);
        return 1;
    } else {
        // The cell is already filled with the segment.
        // Send ack duplicate.
        struct poli_tcp_ctrl_hdr ctrl_segment;
        ctrl_segment.protocol_id = POLI_PROTOCOL_ID;
        ctrl_segment.conn_id = con->conn_id;
        ctrl_segment.ack_num = data_hdr->seq_num;
        ctrl_segment.type = ACK_TYPE;

        // Send ack.
        int rc = 0;
        int clen = sizeof(struct sockaddr_in);
        rc = sendto(con->sockfd, &ctrl_segment, sizeof(ctrl_segment), 0,
            (struct sockaddr *)&con->servaddr, clen);
        if (rc < 0)
            fprintf(stderr, "sendto failed in send_handler!\n");

        return 0;
    }
    return ret;
}

// This function is used to retrieve data from the ring_buffer into
// the segment queue.
// This functions takes the payloads of the segments up the the last
// received one in the ring_buffer and add them to the segment queue of
// the receiver.
int recv_seg_to_queue(struct connection *con) {
    // Retrieve as much as is available.
    while (con->ring_buffer->status == FULL) {
        segment_data *seg_data = (segment_data *)malloc(sizeof(segment_data));
        if (!seg_data) {
            fprintf(stderr, "malloc failed!\n");
            exit(1);
        }

        seg_data->buf = (char *)malloc(MAX_DATA_SIZE * sizeof(char));
        if (!seg_data->buf) {
            fprintf(stderr, "malloc failed!\n");
            exit(1);
        }

        // Add the segment payload to the queue.
        memcpy(seg_data->buf, con->ring_buffer->buf, con->ring_buffer->len);
        seg_data->len = con->ring_buffer->len;
        segment_queues[con->conn_id].push(seg_data);
        
        con->ring_buffer->status = EMPTY;
        con->seq++;
        con->ring_buffer = con->ring_buffer->next;
    }

    return 0;

}

// This function is used to extract the payload of the received segments(chunks from the file)
// from the reveiver's queue into the buffer received as parameter.
int extract_data_buffer(struct connection *con, char *buf, int len) {
    int size = 0;
    while (!segment_queues[con->conn_id].empty()) {
        segment_data *seg_data = segment_queues[con->conn_id].front();

        char *seg_buf = seg_data->buf;
        int seg_len = seg_data->len;

        if (seg_len + size <= len) {
            memcpy(buf + size, seg_buf, seg_len);
            size += seg_len;

            free(seg_buf);
            segment_queues[con->conn_id].pop();
        } else
            break;
    }
    
    return size;
}


// A function created for memory cleaning, sadly it isn't used
// as the current implementation can't use it.
void destroy_buffer(buffer_ring *head) {
    head->prev->next = NULL;
    buffer_ring *curr = head;

    while (curr) {
        buffer_ring *aux = curr;
        curr = curr->next;
        free(aux->buf);
        free(aux);
    }
}
