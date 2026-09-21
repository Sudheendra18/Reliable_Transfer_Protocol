/*
=====================================
Mini Project 1 Submission
Group Details:
Member 1 Name: Harihara Varma
Member 1 Roll number: 23CS10040
Member 2 Name: Nymish Kumar Reddy
Member 2 Roll number: 23CS10074
=====================================
*/

#ifndef KSOCKET_H
#define KSOCKET_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/sem.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef int ktp_fd;
typedef int udp_fd;

#define SOCK_KTP      256
#define MSG_PAYLOAD   512
#define MSG_TYPE_LEN  4
#define HDR_LEN       (MSG_TYPE_LEN + 2 * sizeof(uint16_t))
#define PKT_LEN       (HDR_LEN + MSG_PAYLOAD)
#define SEQ_BITS      5
#define SEQ_SPACE     (1 << SEQ_BITS)
#define SEND_BUF_SZ   10
#define WIN_SZ        SEND_BUF_SZ
#define T             5
#define MAX_KTP_SOCKS 10
#define DROP_PROB     0.3f
#define MAX_FIN_TRIES 5

#define ERR_NO_SPACE  ENOSPC
#define ERR_NOT_BOUND ENOTCONN
#define ERR_NO_MSG    ENOMSG

#define SHM_KEYFILE "/ktp_shm"
#define SHM_PROJ    'K'
#define SEM_KEYFILE "/ktp_sem"
#define SEM_PROJ    'K'

/* sliding window (used for both send and receive sides) */
typedef struct slide_win {
    int      head;               /* oldest unACK'd / next-to-deliver slot */
    uint16_t avail;              /* free slots in window */
    uint16_t seqnums[WIN_SZ];   /* seq number assigned to each slot */
    uint16_t tail_seq;           /* last assigned seq number */
    uint16_t ack_seq;            /* last in-order seq ACK'd (rwnd only) */
    bool     got_pkt[WIN_SZ];   /* slot holds received data (rwnd only) */
    time_t   expires[WIN_SZ];   /* retransmit deadline; -1 = not sent yet (swnd only) */
} slide_win;

/* one KTP socket entry in shared memory */
typedef struct ktp_sock {
    bool               free;
    pid_t              owner_pid;
    udp_fd             udp_sock;
    struct sockaddr_in peer_addr;
    struct sockaddr_in local_addr;
    bool               bound;
    char               sbuf[SEND_BUF_SZ][MSG_PAYLOAD];
    char               rbuf[SEND_BUF_SZ][MSG_PAYLOAD];
    bool               sbuf_empty[SEND_BUF_SZ];
    slide_win          swnd;
    slide_win          rwnd;
    bool               recv_full;   /* rwnd hit 0; probe ACK needed later */
    bool               closed;
    time_t             fin_sent_at; /* -1 if FIN not sent yet */
    int                fin_tries;
} ktp_sock;

union sem_arg {
    int              val;
    struct semid_ds *buf;
    unsigned short  *array;
};

/* user-facing API */
ktp_fd  k_socket(int domain, int type, int protocol);
int     k_bind(ktp_fd fd, const char *src_ip, int src_port, const char *dst_ip, int dst_port);
ssize_t k_sendto(ktp_fd fd, const void *buf, size_t len, int flags, const struct sockaddr *dst, socklen_t dstlen);
ssize_t k_recvfrom(ktp_fd fd, void *buf, size_t len, int flags, struct sockaddr *src, socklen_t *srclen);
int     k_close(ktp_fd fd);

/* internal helpers */
int       get_shmid(void);
ktp_sock *attach_shm(void);
int       detach_shm(ktp_sock *sm);
int       get_semid(void);
void      sem_lock(int semid, ktp_fd i);
void      sem_unlock(int semid, ktp_fd i);
slide_win make_window(void);
bool      sim_drop(float p);

#endif /* KSOCKET_H */