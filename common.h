/**
 * common.h - Shared Definitions
 * 
 * Multi-process Snake Game + Chatroom
 * Architecture: Prefork Server + Shared Memory IPC
 */

#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>

/* ============================================================================
 * Game Constants
 * ============================================================================ */

#define GRID_SIZE        50
#define MAX_PLAYERS      100
#define MAX_SNAKE_LEN    200
#define MAX_FOOD         20
#define MAX_NAME_LEN     16
#define MAX_CHAT_LEN     128
#define MAX_CHAT_HISTORY 50

#define SERVER_PORT      8888
#define GAME_TICK_MS     100
#define NUM_WORKERS      4

#define RESPAWN_TICKS    30   /* 3 seconds */
#define PROTECTION_TICKS 30   /* 3 seconds invincibility */

/* ============================================================================
 * Cell Types
 * ============================================================================ */

#define CELL_EMPTY       0
#define CELL_WALL        1
#define CELL_FOOD        2
#define CELL_SNAKE_BASE  10

/* ============================================================================
 * Directions
 * ============================================================================ */

#define DIR_UP           0
#define DIR_DOWN         1
#define DIR_LEFT         2
#define DIR_RIGHT        3

/* ============================================================================
 * Protocol OpCodes
 * ============================================================================ */

#define OP_LOGIN_REQ     0x0001
#define OP_LOGIN_RESP    0x0002
#define OP_MOVE          0x0003
#define OP_MAP_UPDATE    0x0004
#define OP_CHAT_SEND     0x0005
#define OP_CHAT_RECV     0x0006
#define OP_PLAYER_JOIN   0x0007
#define OP_PLAYER_LEAVE  0x0008
#define OP_PLAYER_DIE    0x0009
#define OP_LOGOUT        0x000A
#define OP_ERROR         0x00FF
#define OP_HEARTBEAT     0x0010
#define OP_HEARTBEAT_ACK 0x0011

/* ============================================================================
 * Protocol Constants
 * ============================================================================ */

#define XOR_KEY          0x5A
#define MAX_PAYLOAD_SIZE 65536

/* ============================================================================
 * Shared Memory
 * ============================================================================ */

#define SHM_KEY_FILE     "/tmp"
#define SHM_KEY_ID       0x5E

/* ============================================================================
 * Data Structures
 * ============================================================================ */

typedef struct {
    int16_t x;
    int16_t y;
} Position;

typedef struct {
    Position body[MAX_SNAKE_LEN];
    int length;
    int head_idx;
    uint8_t direction;
    uint8_t pending_dir;
    bool alive;
} Snake;

typedef struct {
    uint32_t id;
    char name[MAX_NAME_LEN];
    int score;
    uint8_t color;
    bool active;
    bool is_ai;
    int spawn_protection;
    int respawn_timer;
    Snake snake;
} Player;

typedef struct {
    Position pos;
    bool active;
} Food;

typedef struct {
    uint32_t sender_id;
    char sender_name[MAX_NAME_LEN];
    char text[MAX_CHAT_LEN];
    uint64_t timestamp;
} ChatMessage;

/* Packet Header (8 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t length;
    uint16_t opcode;
    uint16_t checksum;
} PacketHeader;

/* Login Request */
typedef struct __attribute__((packed)) {
    char name[MAX_NAME_LEN];
    bool is_ai;
} LoginRequest;

/* Login Response */
typedef struct __attribute__((packed)) {
    uint32_t player_id;
    uint8_t color;
    uint16_t grid_width;
    uint16_t grid_height;
} LoginResponse;

/* Move Command */
typedef struct __attribute__((packed)) {
    uint8_t direction;
} MoveCommand;

/* Map Update */
typedef struct __attribute__((packed)) {
    uint32_t tick;
    uint8_t map[GRID_SIZE][GRID_SIZE];
    int32_t scores[MAX_PLAYERS];
    uint8_t alive[MAX_PLAYERS];
    uint8_t active[MAX_PLAYERS];
    char names[MAX_PLAYERS][MAX_NAME_LEN];
} MapUpdate;

/* Chat Send */
typedef struct __attribute__((packed)) {
    char text[MAX_CHAT_LEN];
} ChatSend;

/* Chat Receive */
typedef struct __attribute__((packed)) {
    uint32_t sender_id;
    char sender_name[MAX_NAME_LEN];
    char text[MAX_CHAT_LEN];
} ChatRecv;

/* ============================================================================
 * Shared Game State (in Shared Memory for IPC)
 * ============================================================================ */

typedef struct {
    /* Synchronization - MUST be first for proper alignment */
    pthread_mutex_t lock;
    pthread_mutexattr_t lock_attr;
    
    /* Map */
    uint8_t map[GRID_SIZE][GRID_SIZE];
    
    /* Players */
    Player players[MAX_PLAYERS];
    int player_count;
    uint32_t next_player_id;
    
    /* Food */
    Food foods[MAX_FOOD];
    int food_count;
    
    /* Chat history (circular buffer) */
    ChatMessage chat_history[MAX_CHAT_HISTORY];
    uint64_t chat_count;  /* Total messages ever (for sync) */
    
    /* Game tick */
    uint64_t tick;
    
    /* Server running flag */
    volatile int running;
} GameState;

#define NUM_COLORS 7

#endif /* COMMON_H */
