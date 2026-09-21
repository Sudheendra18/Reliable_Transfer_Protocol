#include "ksocket.h"

ktp_fd k_socket(int domain, int type, int protocol)
{
    if (domain != AF_INET || type != SOCK_KTP) {
        errno = EINVAL;
        return -1;
    }

    ktp_sock *sm = attach_shm();
    if (sm == NULL) return -1;

    int sid = get_semid();
    if (sid == -1) return -1;

    for (int slot = 0; slot < MAX_KTP_SOCKS; slot++) {
        sem_lock(sid, slot);
        if (sm[slot].free) {
            sm[slot].free        = false;
            sm[slot].bound       = false;
            sm[slot].closed      = false;
            sm[slot].owner_pid   = getpid();
            sm[slot].recv_full   = false;
            sm[slot].fin_tries   = 0;
            sm[slot].fin_sent_at = -1;
            memset(&sm[slot].local_addr, 0, sizeof(sm[slot].local_addr));
            memset(&sm[slot].peer_addr,  0, sizeof(sm[slot].peer_addr));
            for (int b = 0; b < SEND_BUF_SZ; b++)
                sm[slot].sbuf_empty[b] = true;
            sm[slot].swnd = make_window();
            sm[slot].rwnd = make_window();
            printf("k_socket: allocated slot %d\n", slot);
            sem_unlock(sid, slot);
            return slot;
        }
        sem_unlock(sid, slot);
    }

    errno = ERR_NO_SPACE;
    return -1;
}

int k_bind(ktp_fd fd, const char *src_ip, int src_port,
           const char *dst_ip, int dst_port)
{
    ktp_sock *sm = attach_shm();
    if (sm == NULL) return -1;

    int sid = get_semid();
    if (sid == -1) return -1;

    sem_lock(sid, fd);
    sm[fd].local_addr.sin_family      = AF_INET;
    sm[fd].local_addr.sin_port        = htons(src_port);
    sm[fd].local_addr.sin_addr.s_addr = inet_addr(src_ip);
    sm[fd].peer_addr.sin_family       = AF_INET;
    sm[fd].peer_addr.sin_port         = htons(dst_port);
    sm[fd].peer_addr.sin_addr.s_addr  = inet_addr(dst_ip);
    printf("k_bind: slot %d  %s:%d -> %s:%d\n", fd, src_ip, src_port, dst_ip, dst_port);
    sem_unlock(sid, fd);
    return 0;
}

ssize_t k_sendto(ktp_fd fd, const void *buf, size_t len, int flags,
                 const struct sockaddr *dst, socklen_t dstlen)
{
    ktp_sock *sm = attach_shm();
    if (sm == NULL) return -1;

    int sid = get_semid();
    if (sid == -1) return -1;

    sem_lock(sid, fd);

    struct sockaddr_in *peer = (struct sockaddr_in *)dst;
    if (sm[fd].free ||
        sm[fd].peer_addr.sin_addr.s_addr != peer->sin_addr.s_addr ||
        sm[fd].peer_addr.sin_port         != peer->sin_port) {
        errno = ERR_NOT_BOUND;
        sem_unlock(sid, fd);
        return -1;
    }

    int idx = sm[fd].swnd.head;
    for (int c = 0; c < SEND_BUF_SZ; c++, idx = (idx + 1) % SEND_BUF_SZ) {
        if (sm[fd].sbuf_empty[idx]) {
            ssize_t n = (len < MSG_PAYLOAD) ? (ssize_t)len : MSG_PAYLOAD;
            memcpy(sm[fd].sbuf[idx], buf, n);
            memset(sm[fd].sbuf[idx] + n, 0, MSG_PAYLOAD - n);
            sm[fd].sbuf_empty[idx]   = false;
            sm[fd].swnd.expires[idx] = -1;
            printf("k_sendto: slot %d  idx %d  seq %u\n", fd, idx, sm[fd].swnd.seqnums[idx]);
            sem_unlock(sid, fd);
            return n;
        }
    }

    errno = ERR_NO_SPACE;
    sem_unlock(sid, fd);
    return -1;
}

ssize_t k_recvfrom(ktp_fd fd, void *buf, size_t len, int flags,
                   struct sockaddr *src, socklen_t *srclen)
{
    ktp_sock *sm = attach_shm();
    if (sm == NULL) return -1;

    int sid = get_semid();
    if (sid == -1) return -1;

    sem_lock(sid, fd);

    if (sm[fd].free) {
        errno = ERR_NOT_BOUND;
        sem_unlock(sid, fd);
        return -1;
    }

    /* slot just past the window holds the oldest received message */
    int didx = (sm[fd].rwnd.head + sm[fd].rwnd.avail) % WIN_SZ;
    ssize_t ret;

    if (sm[fd].rwnd.got_pkt[didx]) {
        size_t n = (len < MSG_PAYLOAD) ? len : MSG_PAYLOAD;
        memcpy(buf, sm[fd].rbuf[didx], n);
        sm[fd].rwnd.got_pkt[didx] = false;
        sm[fd].rwnd.avail++;
        ret = (ssize_t)strlen((char *)buf);
        printf("k_recvfrom: slot %d  idx %d\n", fd, didx);
    } else {
        errno = ERR_NO_MSG;
        ret = -1;
    }

    sem_unlock(sid, fd);
    return ret;
}

int k_close(ktp_fd fd)
{
    ktp_sock *sm = attach_shm();
    if (sm == NULL) return -1;

    int sid = get_semid();
    if (sid == -1) return -1;

    sem_lock(sid, fd);
    if (!sm[fd].free)
        sm[fd].closed = true;
    printf("k_close: slot %d\n", fd);
    sem_unlock(sid, fd);
    return 0;
}

int get_shmid(void)
{
    key_t k = ftok(SHM_KEYFILE, SHM_PROJ);
    return shmget(k, 0, 0);
}

ktp_sock *attach_shm(void)
{
    int id = get_shmid();
    if (id == -1) return NULL;
    ktp_sock *ptr = (ktp_sock *)shmat(id, NULL, 0);
    return (ptr == (void *)-1) ? NULL : ptr;
}

int detach_shm(ktp_sock *sm)
{
    return shmdt(sm);
}

int get_semid(void)
{
    key_t k = ftok(SEM_KEYFILE, SEM_PROJ);
    return semget(k, 0, 0);
}

void sem_lock(int semid, ktp_fd i)
{
    struct sembuf op = { i, -1, 0 };
    if (semop(semid, &op, 1) == -1) { perror("sem_lock"); exit(1); }
}

void sem_unlock(int semid, ktp_fd i)
{
    struct sembuf op = { i, 1, 0 };
    if (semop(semid, &op, 1) == -1) { perror("sem_unlock"); exit(1); }
}

slide_win make_window(void)
{
    slide_win w;
    w.head     = 0;
    w.avail    = WIN_SZ;
    w.tail_seq = 10;
    w.ack_seq  = 0;
    for (int i = 0; i < WIN_SZ; i++) {
        w.seqnums[i] = (uint16_t)(i + 1);
        w.got_pkt[i] = false;
        w.expires[i] = -1;
    }
    return w;
}

bool sim_drop(float p)
{
    return ((float)rand() / (float)RAND_MAX) < p;
}