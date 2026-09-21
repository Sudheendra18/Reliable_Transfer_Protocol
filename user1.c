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

/* user1.c – sender: reads lorem_100KB.txt and sends it over KTP */

#include "ksocket.h"

#define EOS "~"
static char io_buf[MSG_PAYLOAD];

int main(int argc, char *argv[])
{
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <src_ip> <src_port> <dst_ip> <dst_port>\n", argv[0]);
        return 1;
    }

    const char *src_ip = argv[1]; int src_port = atoi(argv[2]);
    const char *dst_ip = argv[3]; int dst_port = atoi(argv[4]);

    ktp_fd kfd = k_socket(AF_INET, SOCK_KTP, 0);
    if (kfd < 0) { perror("k_socket"); return 1; }

    if (k_bind(kfd, src_ip, src_port, dst_ip, dst_port) < 0) { perror("k_bind"); return 1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(dst_port);
    dst.sin_addr.s_addr = inet_addr(dst_ip);

    FILE *fp = fopen("input.txt", "r");
    if (!fp) { perror("fopen"); return 1; }

    sleep(2); /* let initk bind the UDP socket */

    while (1) {
        size_t got = fread(io_buf, 1, MSG_PAYLOAD, fp);
        if (got == 0) {
            if (!feof(fp)) { perror("fread"); fclose(fp); k_close(kfd); return 1; }
            memcpy(io_buf, EOS, 1);
            got = 1;
            printf("user1: EOF reached, sending EOS\n");
        }

    retry:;
        ssize_t sent = k_sendto(kfd, io_buf, got, 0, (struct sockaddr *)&dst, sizeof(dst));
        if (sent < 0) {
            if (errno == ERR_NO_SPACE || errno == ERR_NOT_BOUND) { sleep(1); goto retry; }
            perror("k_sendto"); fclose(fp); k_close(kfd); return 1;
        }
        printf("user1: queued %zd bytes\n", sent);
        if (memcmp(io_buf, EOS, 1) == 0) break;
    }

    fclose(fp);
    sleep(100); /* wait for retransmission engine to flush */
    k_close(kfd);
    return 0;
}