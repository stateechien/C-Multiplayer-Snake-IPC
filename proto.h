/**
 * proto.h - Protocol Layer
 */

#ifndef PROTO_H
#define PROTO_H

#include "common.h"
#include <stddef.h>

uint16_t calculate_checksum(const unsigned char *data, size_t len);
void xor_cipher(unsigned char *data, size_t len);
int send_packet(int sockfd, uint16_t opcode, const void *payload, uint32_t payload_len);
int recv_packet(int sockfd, uint16_t *opcode, void **payload, uint32_t *payload_len);
int recv_packet_timeout(int sockfd, uint16_t *opcode, void **payload, uint32_t *payload_len, int timeout_ms);

#endif /* PROTO_H */
