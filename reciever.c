// file only including reciever
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define PACKET_SIZE 1024

// packet flags
#define FLAG_SYN  0x01
#define FLAG_ACK  0x02
#define FLAG_FIN  0x04
#define FLAG_DATA 0x08

#pragma pack(push,1)
struct packet_header {
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t checksum;
    uint16_t payload_len;
    uint8_t  flags;
};
#pragma pack(pop)

#define PAYLOAD_SIZE (PACKET_SIZE - sizeof(struct packet_header))

struct packet {
    struct packet_header header;
    char data[PAYLOAD_SIZE];
};

// Checksum 
uint16_t checksum(void *data, int len)
{
    uint32_t  sum = 0;
    uint16_t *ptr = data;

    while (len > 1) { sum += *ptr++; len -= 2; }
    if (len > 0) sum += *((uint8_t *)ptr);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

void compute_checksum(struct packet *pkt)
{
    pkt->header.checksum = 0;
    pkt->header.checksum = checksum(pkt, PACKET_SIZE);
}

int verify_checksum(struct packet *pkt)
{
    uint16_t recv = pkt->header.checksum;
    pkt->header.checksum = 0;
    uint16_t calc = checksum(pkt, PACKET_SIZE);
    pkt->header.checksum = recv;
    return recv == calc;
}

// reciever
void receiver(char *file, int port)
{
    FILE *fp = fopen(file, "wb");
    if (!fp) { perror("fopen"); exit(1); }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }

    struct sockaddr_in addr, client;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }

    uint32_t expected = 0; // next in-order sequence number we want

    while (1) {
        struct packet pkt;
        socklen_t     len = sizeof(client);

        recvfrom(sock, &pkt, sizeof(pkt), 0,
                 (struct sockaddr *)&client, &len);

        // drop corrupted packets silently
        if (!verify_checksum(&pkt)) continue;

        // FIN signals the sender is done — close the loop
        if (pkt.header.flags & FLAG_FIN) {
            printf("FIN received\n");
            break;
        }

        uint32_t seq     = ntohl(pkt.header.seq_num);
        uint16_t payload = ntohs(pkt.header.payload_len);

        struct packet ack;
        memset(&ack, 0, sizeof(ack));
        ack.header.flags = FLAG_ACK;

        if (seq == expected) {
            // in-order packet: write data and advance expected sequence
            fwrite(pkt.data, 1, payload, fp);
            ack.header.ack_num = htonl(seq); // ACK the received sequence number
            expected++;
            printf("recv seq %u\n", seq);
        } else {
            // out-of-order packet: send a duplicate ACK for the last
            // in-order packet — the sender counts 3 of these to trigger
            // fast retransmit without waiting for a timeout
            printf("out of order %u expected %u\n", seq, expected);
            if (expected == 0) continue; // nothing in-order yet, don't send a NAK
            ack.header.ack_num = htonl(expected - 1); // duplicate ACK
        }

        compute_checksum(&ack);
        sendto(sock, &ack, PACKET_SIZE, 0,
               (struct sockaddr *)&client, len);
    }

    fclose(fp);
    close(sock);
}

int main(int argc, char **argv)
{
    int   opt;
    char *file = NULL;
    int   port = 0;

    while ((opt = getopt(argc, argv, "f:p:")) != -1) {
        switch (opt) {
        case 'f': file = optarg;        break;
        case 'p': port = atoi(optarg);  break;
        default:
            printf("Usage: %s -p port -f outfile\n", argv[0]);
            exit(1);
        }
    }

    if (!file || !port) {
        printf("missing args: %s -p port -f outfile\n", argv[0]);
        exit(1);
    }

    receiver(file, port);
    return 0;
}