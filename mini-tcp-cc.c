#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

// networking includes 
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

 
#define PACKET_SIZE 1024
#define WINDOW_SIZE 64   // max window size (cwnd will stay <= this)
#define TIMEOUT_SEC 1
 
// packet flags
#define FLAG_SYN  0x01
#define FLAG_ACK  0x02
#define FLAG_FIN  0x04
#define FLAG_DATA 0x08

// Congestion-control initial values
#define CC_INIT_CWND     1    // start cwnd at 1 MSS (slow start begins here)
#define CC_INIT_SSTHRESH 16   // initial slow-start threshold (in packets)


// packet header structure
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
 
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0)
        sum += *((uint8_t *)ptr);
 
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
 
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
 
// simulates packet loss (rate = 30  →  30% drop probability)
int should_drop(int rate)
{
    return (rand() % 100) < rate;
}

// CC logging helper
// Writes a single-line entry to the log file and also echoes to stdout.
// Format:  EVENT  cwnd  ssthresh  base
static void cc_log(FILE *log, const char *event,
                   double cwnd, uint32_t ssthresh, uint32_t base)
{
    fprintf(log,   "[CC] %-22s  cwnd=%.2f  ssthresh=%u  base=%u\n",
            event, cwnd, ssthresh, base);
    printf(        "[CC] %-22s  cwnd=%.2f  ssthresh=%u  base=%u\n",
            event, cwnd, ssthresh, base);
}


 
// sender
void sender(char *file, char *ip, int port, int loss_rate, int reorder_rate)
{
    FILE *fp = fopen(file, "rb");
    if (!fp) { perror("file"); exit(1); }

    // open (or create) a log file for congestion-control events
    FILE *log = fopen("cc_log.txt", "w");
    if (!log) { perror("cc_log"); exit(1); }
    fprintf(log, "=== Mini-TCP-CC sender log ===\n\n");
 
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }
 
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
 
    // ── Congestion-control state
    double   cwnd     = CC_INIT_CWND;     // congestion window (in packets, float for gradual growth)
    uint32_t ssthresh = CC_INIT_SSTHRESH; // slow-start threshold
    uint32_t dup_ack_count = 0;           // number of consecutive duplicate ACKs received
    uint32_t last_ack      = 0;           // last ACK number seen 
    int      first_ack     = 1;           // flag: haven't received any ACK yet

    // sliding-window bookkeeping
    struct packet window[WINDOW_SIZE];
    uint32_t base    = 0;
    uint32_t nextseq = 0;
    int      eof     = 0;
 
    struct packet reorder_buffer;
    int reorder_valid = 0;
 
    fd_set readfds;
 
    while (!eof || base < nextseq) {
 
        // Fill the window up to min(cwnd, WINDOW_SIZE)
        // cwnd controls how many unacknowledged packets we allow at once.
        uint32_t effective_window = (uint32_t)cwnd;
        if (effective_window < 1)        effective_window = 1;
        if (effective_window > WINDOW_SIZE) effective_window = WINDOW_SIZE;

        while (!eof && nextseq < base + effective_window) {
 
            struct packet pkt;
            memset(&pkt, 0, sizeof(pkt));
 
            int n = fread(pkt.data, 1, PAYLOAD_SIZE, fp);
            if (n <= 0) { eof = 1; break; }
 
            pkt.header.seq_num     = htonl(nextseq);
            pkt.header.payload_len = htons(n);
            pkt.header.flags       = FLAG_DATA;
            compute_checksum(&pkt);
 
            window[nextseq % WINDOW_SIZE] = pkt;
 
            if (should_drop(loss_rate)) {
                printf("drop seq %u\n", nextseq);
            } else if ((rand() % 100) < reorder_rate && !reorder_valid) {
                reorder_buffer = pkt;
                reorder_valid  = 1;
            } else {
                sendto(sock, &pkt, PACKET_SIZE, 0,
                       (struct sockaddr *)&addr, sizeof(addr));
                printf("send seq %u\n", nextseq);
 
                if (reorder_valid) {
                    sendto(sock, &reorder_buffer, PACKET_SIZE, 0,
                           (struct sockaddr *)&addr, sizeof(addr));
                    printf("reordered packet sent\n");
                    reorder_valid = 0;
                }
            }
            nextseq++;
        }
        
        // wait for ACK or timeout
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        struct timeval tv = { TIMEOUT_SEC, 0 };
 
        int rv = select(sock + 1, &readfds, NULL, NULL, &tv);
 
        if (rv == 0) {
            ssthresh = (uint32_t)(cwnd / 2);
            if (ssthresh < 2) ssthresh = 2;   // keep ssthresh >= 2
            cwnd = CC_INIT_CWND;               // back to slow start
            dup_ack_count = 0;                 // reset duplicate-ACK counter

            cc_log(log, "TIMEOUT→SlowStart", cwnd, ssthresh, base);

            // retransmit entire outstanding window
            printf("timeout -> retransmit\n");
            for (uint32_t i = base; i < nextseq; i++) {
                struct packet *pkt = &window[i % WINDOW_SIZE];
                if (!should_drop(loss_rate)) {
                    sendto(sock, pkt, PACKET_SIZE, 0,
                           (struct sockaddr *)&addr, sizeof(addr));
                    printf("retransmit seq %u\n", i);
                }
            }

        } else {
            // Received a packet (should be an ACK)
            struct packet ack;
            socklen_t len = sizeof(addr);
            recvfrom(sock, &ack, sizeof(ack), 0,
                     (struct sockaddr *)&addr, &len);
 
            if (!verify_checksum(&ack)) continue;
 
            if (ack.header.flags & FLAG_ACK) {
                uint32_t acknum = ntohl(ack.header.ack_num);
                printf("ACK %u\n", acknum);

                // Detect duplicate ACK 
                // A duplicate ACK means the receiver got an out-of-order
                // packet and is re-sending the last in-order ACK it issued.
                if (!first_ack && acknum == last_ack) {
                    // same ACK number as before → duplicate
                    dup_ack_count++;
                    cc_log(log, "DUP ACK", cwnd, ssthresh, base);

                    if (dup_ack_count == 3) {
                        // ── Fast Retransmit (3 duplicate ACKs) ───────────
                        // TCP Reno: halve ssthresh & cwnd, immediately
                        // retransmit the missing packet without waiting for
                        // a timeout.
                        ssthresh = (uint32_t)(cwnd / 2);
                        if (ssthresh < 2) ssthresh = 2;
                        cwnd = ssthresh;           // set cwnd = ssthresh (TCP Reno)
                        dup_ack_count = 0;

                        cc_log(log, "FAST_RETRANSMIT", cwnd, ssthresh, base);

                        // retransmit the first unacknowledged packet
                        uint32_t retrans_seq = base;
                        struct packet *rpkt = &window[retrans_seq % WINDOW_SIZE];
                        sendto(sock, rpkt, PACKET_SIZE, 0,
                               (struct sockaddr *)&addr, sizeof(addr));
                        printf("fast retransmit seq %u\n", retrans_seq);
                    }
                } else {
                    // new ACK (not a duplicate)
                    dup_ack_count = 0;  // reset duplicate counter on any progress

                    if (cwnd < ssthresh) {
                        // slow start
                        // Double cwnd each RTT by incrementing by 1 per ACK.
                        // Exponential growth until ssthresh is reached.
                        cwnd += 1.0;
                        cc_log(log, "SLOW_START", cwnd, ssthresh, base);
                    } else {
                        // congestion avoidance 
                        // Linear growth: increase cwnd by 1/cwnd per ACK,
                        // which adds roughly 1 MSS per full RTT (TCP AIMD).
                        cwnd += 1.0 / cwnd;
                        cc_log(log, "CONG_AVOIDANCE", cwnd, ssthresh, base);
                    }

                    // cap cwnd so we never exceed our static buffer
                    if (cwnd > WINDOW_SIZE) cwnd = WINDOW_SIZE;

                    if (acknum >= base)
                        base = acknum + 1;

                    last_ack  = acknum;
                    first_ack = 0;
                }
            }
        }
    }
 
    // send fin
    struct packet fin;
    memset(&fin, 0, sizeof(fin));
    fin.header.flags = FLAG_FIN;
    compute_checksum(&fin);
    sendto(sock, &fin, PACKET_SIZE, 0,
           (struct sockaddr *)&addr, sizeof(addr));
    printf("FIN sent\n");

    fprintf(log, "\n=== Transfer complete ===\n");
    fclose(log);
    fclose(fp);
    close(sock);
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
 
    uint32_t expected = 0;
 
    while (1) {
        struct packet  pkt;
        socklen_t      len = sizeof(client);
 
        recvfrom(sock, &pkt, sizeof(pkt), 0,
                 (struct sockaddr *)&client, &len);
 
        if (!verify_checksum(&pkt)) continue;
 
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
            fwrite(pkt.data, 1, payload, fp);
            ack.header.ack_num = htonl(seq);
            expected++;
            printf("recv seq %u\n", seq);
        } else {
            // Out-of-order packet: send a duplicate ACK for the last
            // in-order packet.  The sender uses 3 duplicate ACKs to
            // trigger fast retransmit.
            printf("out of order %u expected %u\n", seq, expected);
            if (expected == 0) continue;
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
    srand(time(NULL));
 
    int   opt;
    int   sender_mode   = 0;
    int   receiver_mode = 0;
    char *file          = NULL;
    char *ip            = NULL;
    int   port          = 0;
    int   loss          = 0;
    int   reorder       = 0;
 
    while ((opt = getopt(argc, argv, "SRf:l:r:p:i:")) != -1) {
        switch (opt) {
        case 'S': sender_mode   = 1;             break;
        case 'R': receiver_mode = 1;             break;
        case 'f': file          = optarg;        break;
        case 'l': loss          = atoi(optarg);  break;
        case 'r': reorder       = atoi(optarg);  break;
        case 'p': port          = atoi(optarg);  break;
        case 'i': ip            = optarg;        break;
        default:
            printf("Sender:   %s -S -f file -l loss%% -r reorder%% -p port -i ip\n"
                   "Receiver: %s -R -p port -f outfile\n", argv[0], argv[0]);
            exit(1);
        }
    }
 
    if (sender_mode) {
        if (!file || !ip || !port) { printf("sender args missing\n"); exit(1); }
        sender(file, ip, port, loss, reorder);
    } else if (receiver_mode) {
        if (!file || !port) { printf("receiver args missing\n"); exit(1); }
        receiver(file, port);
    } else {
        printf("must choose -S or -R\n");
    }
 
    return 0;
}