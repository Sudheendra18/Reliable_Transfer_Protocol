/*
 * Packet format (PKT_LEN bytes total):
 *   [0..3]  type : "DATA", "ACK\0", "FIN\0", "FAK\0"
 *   [4..5]  seq  : big-endian uint16
 *   [6..7]  winsz: big-endian uint16  (0 for DATA/FIN/FAK)
 *   [8..]   payload: MSG_PAYLOAD bytes (zeroed for ACK/FIN/FAK)
 */

#include "ksocket.h"

#define SEL_TIMEOUT_US 100000

/* remove a slot from the monitored fd set and free it */
#define release_slot(sm, i) \
    do { FD_CLR((sm)[i].udp_sock, &watched_fds); \
         close((sm)[i].udp_sock); \
         (sm)[i].free = true; \
         printf("released slot %d\n", i); } while (0)

static fd_set watched_fds;

/* ---- packet helpers ---- */

static void parse_pkt(const char *raw, char tag[MSG_TYPE_LEN + 1],
                      uint16_t *seq, uint16_t *winsz, char payload[MSG_PAYLOAD])
{
    memcpy(tag, raw, MSG_TYPE_LEN);
    tag[MSG_TYPE_LEN] = '\0';
    *seq   = ntohs(*(uint16_t *)(raw + MSG_TYPE_LEN));
    *winsz = ntohs(*(uint16_t *)(raw + MSG_TYPE_LEN + sizeof(uint16_t)));
    memcpy(payload, raw + HDR_LEN, MSG_PAYLOAD);
}

static void build_ack(char buf[PKT_LEN], uint16_t seq, uint16_t winsz)
{
    memset(buf, 0, PKT_LEN);
    memcpy(buf, "ACK\0", MSG_TYPE_LEN);
    uint16_t ns = htons(seq), nw = htons(winsz);
    memcpy(buf + MSG_TYPE_LEN, &ns, sizeof(uint16_t));
    memcpy(buf + MSG_TYPE_LEN + sizeof(uint16_t), &nw, sizeof(uint16_t));
}

static void build_data(char buf[PKT_LEN], uint16_t seq, const char payload[MSG_PAYLOAD])
{
    memset(buf, 0, PKT_LEN);
    memcpy(buf, "DATA", MSG_TYPE_LEN);
    uint16_t ns = htons(seq), nw = htons(0);
    memcpy(buf + MSG_TYPE_LEN, &ns, sizeof(uint16_t));
    memcpy(buf + MSG_TYPE_LEN + sizeof(uint16_t), &nw, sizeof(uint16_t));
    memcpy(buf + HDR_LEN, payload, MSG_PAYLOAD);
}

static void build_ctrl(char buf[PKT_LEN], const char *type)
{
    memset(buf, 0, PKT_LEN);
    memcpy(buf, type, MSG_TYPE_LEN);
}

static ssize_t tx_ack(udp_fd sock, struct sockaddr_in dst, uint16_t seq, uint16_t winsz)
{
    char pkt[PKT_LEN];
    build_ack(pkt, seq, winsz);
    return sendto(sock, pkt, PKT_LEN, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static ssize_t tx_data(udp_fd sock, struct sockaddr_in dst, uint16_t seq, const char payload[MSG_PAYLOAD])
{
    char pkt[PKT_LEN];
    build_data(pkt, seq, payload);
    return sendto(sock, pkt, PKT_LEN, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static ssize_t tx_ctrl(udp_fd sock, struct sockaddr_in dst, const char *type)
{
    char pkt[PKT_LEN];
    build_ctrl(pkt, type);
    return sendto(sock, pkt, PKT_LEN, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ---- IPC setup ---- */

static void create_shm(void)
{
    key_t k = ftok(SHM_KEYFILE, SHM_PROJ);
    int id  = shmget(k, MAX_KTP_SOCKS * sizeof(ktp_sock), IPC_CREAT | IPC_EXCL | 0777);
    if (id == -1) { perror("shmget"); exit(1); }
    ktp_sock *sm = (ktp_sock *)shmat(id, NULL, 0);
    if (sm == (void *)-1) { perror("shmat"); exit(1); }
    for (int i = 0; i < MAX_KTP_SOCKS; i++) sm[i].free = true;
    shmdt(sm);
    printf("shm created id=%d\n", id);
}

static void create_sem(void)
{
    key_t k = ftok(SEM_KEYFILE, SEM_PROJ);
    int id  = semget(k, MAX_KTP_SOCKS, IPC_CREAT | IPC_EXCL | 0777);
    if (id == -1) { perror("semget"); exit(1); }
    unsigned short vals[MAX_KTP_SOCKS];
    for (int i = 0; i < MAX_KTP_SOCKS; i++) vals[i] = 1;
    union sem_arg arg; arg.array = vals;
    if (semctl(id, 0, SETALL, arg) == -1) { perror("semctl"); exit(1); }
    printf("sem created id=%d\n", id);
}

static void cleanup_ipc(int signo)
{
    int shid = get_shmid(), seid = get_semid();
    if (shid != -1) shmctl(shid, IPC_RMID, NULL);
    if (seid != -1) semctl(seid, 0, IPC_RMID);
    if (signo == SIGSEGV) fprintf(stderr, "SIGSEGV caught\n");
    exit(0);
}

/* ---- Thread R: receive / ACK handler ---- */

void *threadR(void *arg)
{
    (void)arg;
    fd_set ready; udp_fd hi = 0; struct timeval tv;
    char raw[PKT_LEN];
    FD_ZERO(&watched_fds);
    ktp_sock *sm = attach_shm();
    int sid = get_semid();

    for (;;) {
        ready = watched_fds;
        tv.tv_sec = 0; tv.tv_usec = SEL_TIMEOUT_US;
        select(hi + 1, &ready, NULL, NULL, &tv);

        /* find a socket with incoming data */
        int active = -1; ssize_t nb = -1;
        struct sockaddr_in sender; socklen_t slen = sizeof(sender);
        for (int i = 0; i < MAX_KTP_SOCKS; i++) {
            sem_lock(sid, i);
            if (!sm[i].free && sm[i].bound && FD_ISSET(sm[i].udp_sock, &ready)) {
                active = sm[i].udp_sock;
                nb = recvfrom(sm[i].udp_sock, raw, PKT_LEN, 0,
                              (struct sockaddr *)&sender, &slen);
            }
            sem_unlock(sid, i);
            if (active != -1) break;
        }

        if (active != -1) {
            if (nb <= 0) {
                for (int i = 0; i < MAX_KTP_SOCKS; i++) {
                    sem_lock(sid, i);
                    if (!sm[i].free && sm[i].bound && sm[i].udp_sock == active)
                        sm[i].closed = true;
                    sem_unlock(sid, i);
                }
                continue;
            }

            for (int i = 0; i < MAX_KTP_SOCKS; i++) {
                sem_lock(sid, i);
                if (!sm[i].free && sm[i].bound &&
                    sm[i].udp_sock == active &&
                    sm[i].peer_addr.sin_addr.s_addr == sender.sin_addr.s_addr &&
                    sm[i].peer_addr.sin_port == sender.sin_port)
                {
                    char tag[MSG_TYPE_LEN + 1], payload[MSG_PAYLOAD];
                    uint16_t seq, winsz;
                    parse_pkt(raw, tag, &seq, &winsz, payload);

                    if (sim_drop(DROP_PROB)) {
                        printf("R: dropped %s seq=%u slot=%d\n", tag, seq, i);
                        sem_unlock(sid, i);
                        continue;
                    }

                    if (strcmp(tag, "DATA") == 0) {
                        printf("R: DATA seq=%u slot=%d\n", seq, i);
                        sm[i].recv_full = false;
                        bool dup = true;

                        int wi = sm[i].rwnd.head;
                        for (int c = 0; c < (int)sm[i].rwnd.avail; c++, wi = (wi+1)%WIN_SZ) {
                            if (sm[i].rwnd.seqnums[wi] != seq) continue;
                            if (!sm[i].rwnd.got_pkt[wi]) {
                                dup = false;
                                sm[i].rwnd.got_pkt[wi] = true;
                                memcpy(sm[i].rbuf[wi], payload, MSG_PAYLOAD);

                                /* slide window over consecutive received messages */
                                int last = -1, ck = sm[i].rwnd.head;
                                for (int ct = 0; ct < (int)sm[i].rwnd.avail; ct++, ck = (ck+1)%WIN_SZ) {
                                    if (!sm[i].rwnd.got_pkt[ck]) break;
                                    last = ck;
                                }
                                if (last != -1) {
                                    sm[i].rwnd.ack_seq = sm[i].rwnd.seqnums[last];
                                    int ck2 = sm[i].rwnd.head;
                                    for (;;) {
                                        sm[i].rwnd.seqnums[ck2] = sm[i].rwnd.tail_seq % SEQ_SPACE + 1;
                                        sm[i].rwnd.tail_seq = sm[i].rwnd.seqnums[ck2];
                                        if (ck2 == last) break;
                                        ck2 = (ck2 + 1) % WIN_SZ;
                                    }
                                    int adv = (last - sm[i].rwnd.head + WIN_SZ) % WIN_SZ + 1;
                                    sm[i].rwnd.avail -= adv;
                                    sm[i].rwnd.head   = (last + 1) % WIN_SZ;
                                    printf("R: ACK seq=%u winsz=%u slot=%d\n",
                                           sm[i].rwnd.ack_seq, sm[i].rwnd.avail, i);
                                    if (tx_ack(sm[i].udp_sock, sm[i].peer_addr,
                                               sm[i].rwnd.ack_seq, sm[i].rwnd.avail) < 0)
                                        perror("R: tx_ack");
                                }
                            }
                            break;
                        }

                        if (dup) {
                            printf("R: dup seq=%u re-ACK seq=%u slot=%d\n",
                                   seq, sm[i].rwnd.ack_seq, i);
                            if (tx_ack(sm[i].udp_sock, sm[i].peer_addr,
                                       sm[i].rwnd.ack_seq, sm[i].rwnd.avail) < 0)
                                perror("R: tx_ack dup");
                        }
                        if (sm[i].rwnd.avail == 0) sm[i].recv_full = true;
                    }
                    else if (strcmp(tag, "ACK") == 0) {
                        printf("R: ACK seq=%u winsz=%u slot=%d\n", seq, winsz, i);
                        /* slide swnd: free all slots up to seq */
                        int wi2 = sm[i].swnd.head;
                        for (int c = 0; c < (int)sm[i].swnd.avail; c++, wi2 = (wi2+1)%WIN_SZ) {
                            if (sm[i].swnd.seqnums[wi2] != seq) continue;
                            int ck = sm[i].swnd.head;
                            for (;;) {
                                sm[i].swnd.expires[ck]  = -1;
                                sm[i].sbuf_empty[ck]    = true;
                                sm[i].swnd.seqnums[ck]  = sm[i].swnd.tail_seq % SEQ_SPACE + 1;
                                sm[i].swnd.tail_seq     = sm[i].swnd.seqnums[ck];
                                if (ck == wi2) break;
                                ck = (ck + 1) % WIN_SZ;
                            }
                            sm[i].swnd.head = (wi2 + 1) % WIN_SZ;
                            break;
                        }
                        sm[i].swnd.avail = winsz;
                    }
                    else if (strcmp(tag, "FIN") == 0) {
                        printf("R: FIN slot=%d\n", i);
                        if (tx_ctrl(sm[i].udp_sock, sm[i].peer_addr, "FAK\0") < 0)
                            perror("R: tx_fak");
                        release_slot(sm, i);
                    }
                    else if (strcmp(tag, "FAK") == 0) {
                        printf("R: FAK slot=%d\n", i);
                        release_slot(sm, i);
                    }
                }
                sem_unlock(sid, i);
            }
        }
        else {
            /* timeout: bind new sockets, send probe ACK if window reopened */
            for (int i = 0; i < MAX_KTP_SOCKS; i++) {
                sem_lock(sid, i);
                if (!sm[i].free) {
                    if (!sm[i].bound) {
                        udp_fd ufd = socket(AF_INET, SOCK_DGRAM, 0);
                        if (ufd < 0) {
                            perror("R: socket");
                        } else if (bind(ufd, (struct sockaddr *)&sm[i].local_addr,
                                        sizeof(sm[i].local_addr)) < 0) {
                            perror("R: bind"); close(ufd);
                        } else {
                            sm[i].udp_sock = ufd;
                            sm[i].bound    = true;
                            FD_SET(ufd, &watched_fds);
                            if (ufd > hi) hi = ufd;
                            printf("R: slot %d bound to udp_fd %d\n", i, ufd);
                        }
                    } else if (sm[i].recv_full && sm[i].rwnd.avail > 0) {
                        /* window re-opened; notify sender */
                        printf("R: probe ACK slot=%d winsz=%u\n", i, sm[i].rwnd.avail);
                        if (tx_ack(sm[i].udp_sock, sm[i].peer_addr,
                                   sm[i].rwnd.ack_seq, sm[i].rwnd.avail) < 0)
                            perror("R: probe tx_ack");
                        sm[i].recv_full = false;
                    }
                }
                sem_unlock(sid, i);
            }
        }
    }
    return NULL;
}

/* ---- Thread S: sender / retransmission timer ---- */

void *threadS(void *arg)
{
    (void)arg;
    ktp_sock *sm = attach_shm();
    int sid = get_semid();

    for (;;) {
        sleep(T / 2);

        /* pass 1: retransmit timed-out packets; drive FIN handshake */
        for (int i = 0; i < MAX_KTP_SOCKS; i++) {
            sem_lock(sid, i);
            if (!sm[i].free && sm[i].bound) {
                if (sm[i].closed) {
                    if (sm[i].fin_sent_at == -1) {
                        printf("S: FIN slot=%d\n", i);
                        if (tx_ctrl(sm[i].udp_sock, sm[i].peer_addr, "FIN\0") < 0)
                            perror("S: tx_fin");
                        sm[i].fin_sent_at = time(NULL);
                    } else if (time(NULL) - sm[i].fin_sent_at >= T) {
                        if (sm[i].fin_tries < MAX_FIN_TRIES) {
                            printf("S: FIN retry %d slot=%d\n", ++sm[i].fin_tries, i);
                            if (tx_ctrl(sm[i].udp_sock, sm[i].peer_addr, "FIN\0") < 0)
                                perror("S: tx_fin retry");
                            sm[i].fin_sent_at = time(NULL);
                        } else {
                            printf("S: FAK timeout, closing slot=%d\n", i);
                            release_slot(sm, i);
                        }
                    }
                } else {
                    time_t oldest = sm[i].swnd.expires[sm[i].swnd.head];
                    if (oldest > 0 && (time(NULL) - oldest) >= T) {
                        int wi = sm[i].swnd.head;
                        for (int c = 0; c < (int)sm[i].swnd.avail; c++, wi = (wi+1)%WIN_SZ) {
                            if (sm[i].swnd.expires[wi] == -1) break;
                            printf("S: retransmit seq=%u slot=%d\n", sm[i].swnd.seqnums[wi], i);
                            if (tx_data(sm[i].udp_sock, sm[i].peer_addr,
                                        sm[i].swnd.seqnums[wi], sm[i].sbuf[wi]) < 0)
                                perror("S: tx_data retx");
                            sm[i].swnd.expires[wi] = time(NULL) + T;
                        }
                    }
                }
            }
            sem_unlock(sid, i);
        }

        /* pass 2: send unsent buffered messages */
        for (int i = 0; i < MAX_KTP_SOCKS; i++) {
            sem_lock(sid, i);
            if (!sm[i].free && sm[i].bound) {
                int wi = sm[i].swnd.head;
                for (int c = 0; c < (int)sm[i].swnd.avail; c++, wi = (wi+1)%WIN_SZ) {
                    if (sm[i].swnd.expires[wi] != -1) continue;
                    if (sm[i].sbuf_empty[wi])          continue;
                    printf("S: send seq=%u slot=%d\n", sm[i].swnd.seqnums[wi], i);
                    if (tx_data(sm[i].udp_sock, sm[i].peer_addr,
                                sm[i].swnd.seqnums[wi], sm[i].sbuf[wi]) < 0)
                        perror("S: tx_data");
                    sm[i].swnd.expires[wi] = time(NULL) + T;
                }
            }
            sem_unlock(sid, i);
        }
    }
    return NULL;
}

/* ---- Thread G: garbage collector ---- */

void *threadG(void *arg)
{
    (void)arg;
    ktp_sock *sm = attach_shm();
    int sid = get_semid();

    for (;;) {
        sleep(T);
        for (int i = 0; i < MAX_KTP_SOCKS; i++) {
            sem_lock(sid, i);
            if (!sm[i].free && !sm[i].closed) {
                if (kill(sm[i].owner_pid, 0) == -1) {
                    printf("G: pid %d gone, slot=%d\n", sm[i].owner_pid, i);
                    sm[i].closed = true;
                }
            }
            sem_unlock(sid, i);
        }
    }
    return NULL;
}

int main(void)
{
    srand((unsigned)time(NULL));
    create_shm();
    create_sem();
    FD_ZERO(&watched_fds);
    signal(SIGINT,  cleanup_ipc);
    signal(SIGSEGV, cleanup_ipc);

    pthread_t r_tid, s_tid, g_tid;
    pthread_create(&r_tid, NULL, threadR, NULL);
    pthread_create(&s_tid, NULL, threadS, NULL);
    pthread_create(&g_tid, NULL, threadG, NULL);
    pthread_exit(NULL);
}