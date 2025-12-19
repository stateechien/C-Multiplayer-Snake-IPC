/**
 * proto.c - Protocol Implementation
 */

#include "proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>

uint16_t calculate_checksum(const unsigned char *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint16_t)(sum & 0xFFFF);
}

void xor_cipher(unsigned char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] ^= XOR_KEY;
    }
}

int send_packet(int sockfd, uint16_t opcode, const void *payload, uint32_t payload_len) {
    PacketHeader header;
    unsigned char *buffer = NULL;
    
    if (payload_len > 0 && payload != NULL) {
        buffer = (unsigned char *)malloc(payload_len);
        if (!buffer) return -1;
        memcpy(buffer, payload, payload_len);
        
        header.checksum = htons(calculate_checksum((unsigned char*)payload, payload_len));
        xor_cipher(buffer, payload_len);
    } else {
        header.checksum = 0;
    }

    header.length = htonl(payload_len);
    header.opcode = htons(opcode);

    ssize_t sent = send(sockfd, &header, sizeof(header), MSG_NOSIGNAL);
    if (sent != sizeof(header)) {
        free(buffer);
        return -1;
    }

    if (payload_len > 0 && buffer != NULL) {
        size_t total_sent = 0;
        while (total_sent < payload_len) {
            ssize_t n = send(sockfd, buffer + total_sent, payload_len - total_sent, MSG_NOSIGNAL);
            if (n <= 0) {
                free(buffer);
                return -1;
            }
            total_sent += n;
        }
        free(buffer);
    }

    return 0;
}

int recv_packet(int sockfd, uint16_t *opcode, void **payload, uint32_t *payload_len) {
    PacketHeader header;
    
    ssize_t received = recv(sockfd, &header, sizeof(header), MSG_WAITALL);
    if (received <= 0) return -1;
    if (received != sizeof(header)) return -1;

    uint32_t len = ntohl(header.length);
    *opcode = ntohs(header.opcode);
    uint16_t received_checksum = ntohs(header.checksum);
    *payload_len = len;

    if (len > MAX_PAYLOAD_SIZE) return -1;

    if (len > 0) {
        *payload = malloc(len);
        if (!*payload) return -1;

        size_t total_received = 0;
        while (total_received < len) {
            ssize_t r = recv(sockfd, (unsigned char*)*payload + total_received, 
                           len - total_received, 0);
            if (r <= 0) {
                free(*payload);
                *payload = NULL;
                return -1;
            }
            total_received += r;
        }

        xor_cipher((unsigned char*)*payload, len);

        uint16_t calc_checksum = calculate_checksum((unsigned char*)*payload, len);
        if (calc_checksum != received_checksum) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    } else {
        *payload = NULL;
    }

    return 0;
}

int recv_packet_timeout(int sockfd, uint16_t *opcode, void **payload, 
                        uint32_t *payload_len, int timeout_ms) {
    fd_set readfds;
    struct timeval tv;
    
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    int ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    
    if (ret == 0) return -2;
    if (ret < 0) return -1;
    
    return recv_packet(sockfd, opcode, payload, payload_len);
}
