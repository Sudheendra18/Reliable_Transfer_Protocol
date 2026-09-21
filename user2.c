/* user2.c – receiver: writes received data to received_<port>.txt */

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

    char fname[128];
    snprintf(fname, sizeof(fname), "received_%d.txt", src_port);
    FILE *fp = fopen(fname, "w");
    if (!fp) { perror("fopen"); return 1; }

    sleep(2); /* let initk bind the UDP socket */

    while (1) {
        ssize_t n = k_recvfrom(kfd, io_buf, MSG_PAYLOAD, 0, NULL, NULL);
        if (n < 0) {
            if (errno == ERR_NO_MSG) { sleep(1); continue; }
            perror("k_recvfrom"); fclose(fp); k_close(kfd); return 1;
        }
        printf("user2: got %zd bytes\n", n);
        if (memcmp(io_buf, EOS, 1) == 0) {
            printf("user2: EOS received, done\n");
            break;
        }
        fwrite(io_buf, 1, (size_t)n, fp);
        fflush(fp);
    }

    fclose(fp);
    k_close(kfd);
    return 0;
}