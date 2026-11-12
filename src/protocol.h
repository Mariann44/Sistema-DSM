/* protocol.h - formato de mensajes entre nodos (Entregable 1, "Protocolo de mensajes"). */
#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

#define DSM_MAGIC 0x4453
#define DSM_PROTO_VERSION 1
#define MAX_PAYLOAD (1u << 20)
#define NODE_UNKNOWN 0xFFFF

enum msg_type {
    MSG_HELLO = 0x01, MSG_JOIN_REQUEST = 0x02, MSG_JOIN_ACK = 0x03,
    MSG_NODE_JOINED = 0x04, MSG_NODE_LEAVE = 0x05, MSG_HEARTBEAT = 0x06,
    MSG_PAGE_REQUEST = 0x10, MSG_PAGE_DATA = 0x11, MSG_PAGE_FETCH = 0x12,
    MSG_PAGE_FETCH_REPLY = 0x13, MSG_INVALIDATE = 0x14, MSG_INVALIDATE_ACK = 0x15,
    MSG_PAGE_WRITEBACK = 0x16, MSG_WRITEBACK_ACK = 0x17,
    MSG_ALLOC_COMMIT = 0x22,
    MSG_LOCK_ACQUIRE = 0x30, MSG_LOCK_GRANT = 0x31, MSG_LOCK_RELEASE = 0x32,
    MSG_STATS_REQUEST = 0x40, MSG_STATS_REPLY = 0x41,
    MSG_PAGES_REQUEST = 0x42, MSG_PAGES_REPLY = 0x43,
    MSG_ERROR = 0x7F
};
#define F_WRITE       0x0001
#define F_HAS_COPY    0x0002
#define F_HAS_DATA    0x0004
#define F_FETCH_INVAL 0x0008

typedef struct {
    uint16_t magic; uint8_t version; uint8_t type;
    uint16_t src_node; uint16_t flags;
    uint32_t request_id; uint32_t page_id;
    int16_t status; uint16_t reserved; uint32_t payload_len;
} msg_header_t;
_Static_assert(sizeof(msg_header_t) == 24, "encabezado de 24 bytes");

/* Serialización manual de enteros (orden de red) para payloads. */
static inline void put16(uint8_t *b, uint16_t v) { v = htons(v); memcpy(b, &v, 2); }
static inline void put32(uint8_t *b, uint32_t v) { v = htonl(v); memcpy(b, &v, 4); }
static inline void put64(uint8_t *b, uint64_t v) { v = htobe64(v); memcpy(b, &v, 8); }
static inline uint16_t get16(const uint8_t *b) { uint16_t v; memcpy(&v, b, 2); return ntohs(v); }
static inline uint32_t get32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return ntohl(v); }
static inline uint64_t get64(const uint8_t *b) { uint64_t v; memcpy(&v, b, 8); return be64toh(v); }

#define JOIN_REQ_LEN   28
/* node_wire_t (32 B): node_id u16, state u8, slot u8, ipv4 u32, data/hb/ctl/http_port u16 x4,
 * first_page u32, num_pages u32, free_pages u32, os_pid u32. (Entregable 1: 28 B; se usa el byte
 * reservado para el slot y se agrega os_pid al final.) */
#define NODE_WIRE_LEN  32
/* alloc_wire_t (32 B): alloc_id u64, size_bytes u64, dsm_pid u32, first_page u32, num_pages u32, reserved u32 */
#define ALLOC_WIRE_LEN 32
#define HEARTBEAT_LEN  16  /* state u8, reserved u8[3], free_pages u32, timestamp_ms u64 */
#define PAGE_INFO_WIRE_LEN 16 /* page u32, state u8, allocated u8, owner u16, version u32, home u16, reserved u16 */
#endif
