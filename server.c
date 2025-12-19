/**
 * server.c - Multi-Process Snake Game + Chatroom Server
 * 
 * Architecture:
 * - Master Process: Accept connections, dispatch to workers
 * - Game Loop Process: Update game state, move snakes, auto-respawn
 * - Worker Processes (Prefork): Handle client I/O
 * 
 * IPC: System V Shared Memory with process-shared mutex
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>

#include "common.h"
#include "proto.h"

/* ============================================================================
 * Global Variables
 * ============================================================================ */

static int g_shmid = -1;
static GameState *g_state = NULL;
static int g_server_fd = -1;
static pid_t g_workers[NUM_WORKERS];
static pid_t g_game_loop_pid = 0;
static volatile int g_running = 1;

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

static void msleep(int ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static uint64_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ============================================================================
 * Game Initialization
 * ============================================================================ */

static void init_map(void) {
    memset(g_state->map, CELL_EMPTY, sizeof(g_state->map));
    
    for (int x = 0; x < GRID_SIZE; x++) {
        g_state->map[0][x] = CELL_WALL;
        g_state->map[GRID_SIZE - 1][x] = CELL_WALL;
    }
    for (int y = 0; y < GRID_SIZE; y++) {
        g_state->map[y][0] = CELL_WALL;
        g_state->map[y][GRID_SIZE - 1] = CELL_WALL;
    }
}

static void spawn_food(void) {
    if (g_state->food_count >= MAX_FOOD) return;
    
    for (int attempt = 0; attempt < 100; attempt++) {
        int x = 1 + rand() % (GRID_SIZE - 2);
        int y = 1 + rand() % (GRID_SIZE - 2);
        
        if (g_state->map[y][x] == CELL_EMPTY) {
            for (int i = 0; i < MAX_FOOD; i++) {
                if (!g_state->foods[i].active) {
                    g_state->foods[i].pos.x = x;
                    g_state->foods[i].pos.y = y;
                    g_state->foods[i].active = true;
                    g_state->food_count++;
                    return;
                }
            }
        }
    }
}

static bool find_spawn_pos(int *out_x, int *out_y) {
    for (int attempt = 0; attempt < 100; attempt++) {
        int x = 5 + rand() % (GRID_SIZE - 10);
        int y = 5 + rand() % (GRID_SIZE - 10);
        
        bool clear = true;
        for (int dy = -2; dy <= 2 && clear; dy++) {
            for (int dx = -2; dx <= 2 && clear; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx < 1 || nx >= GRID_SIZE-1 || ny < 1 || ny >= GRID_SIZE-1) continue;
                uint8_t cell = g_state->map[ny][nx];
                if (cell != CELL_EMPTY && cell != CELL_FOOD) {
                    clear = false;
                }
            }
        }
        
        if (clear) {
            *out_x = x;
            *out_y = y;
            return true;
        }
    }
    
    *out_x = GRID_SIZE / 2;
    *out_y = GRID_SIZE / 2;
    return false;
}

static void init_snake(Player *player, int spawn_x, int spawn_y) {
    Snake *s = &player->snake;
    memset(s, 0, sizeof(Snake));
    
    s->direction = DIR_RIGHT;
    s->pending_dir = DIR_RIGHT;
    s->alive = true;
    s->length = 3;
    s->head_idx = 2;
    
    s->body[2].x = spawn_x;
    s->body[2].y = spawn_y;
    s->body[1].x = spawn_x - 1;
    s->body[1].y = spawn_y;
    s->body[0].x = spawn_x - 2;
    s->body[0].y = spawn_y;
    
    player->spawn_protection = PROTECTION_TICKS;
    player->respawn_timer = 0;
}

static void add_chat_message(uint32_t sender_id, const char *sender_name, const char *text) {
    int idx = g_state->chat_count % MAX_CHAT_HISTORY;
    
    g_state->chat_history[idx].sender_id = sender_id;
    strncpy(g_state->chat_history[idx].sender_name, sender_name, MAX_NAME_LEN - 1);
    g_state->chat_history[idx].sender_name[MAX_NAME_LEN - 1] = '\0';
    strncpy(g_state->chat_history[idx].text, text, MAX_CHAT_LEN - 1);
    g_state->chat_history[idx].text[MAX_CHAT_LEN - 1] = '\0';
    g_state->chat_history[idx].timestamp = get_time_ms();
    
    g_state->chat_count++;
    
    printf("[CHAT] %s: %s\n", sender_name, text);
}

static void init_game_state(void) {
    memset(g_state, 0, sizeof(GameState));
    
    pthread_mutexattr_init(&g_state->lock_attr);
    pthread_mutexattr_setpshared(&g_state->lock_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&g_state->lock, &g_state->lock_attr);
    
    g_state->next_player_id = 1;
    g_state->running = 1;
    
    init_map();
    
    for (int i = 0; i < MAX_FOOD / 2; i++) {
        spawn_food();
    }
}

/* ============================================================================
 * Game Logic
 * ============================================================================ */

static Position snake_head(const Snake *s) {
    return s->body[s->head_idx];
}

static void move_snake(Snake *s) {
    if (!s->alive) return;
    
    int opposite[4] = { DIR_DOWN, DIR_UP, DIR_RIGHT, DIR_LEFT };
    if (s->pending_dir != opposite[s->direction]) {
        s->direction = s->pending_dir;
    }
    
    Position head = snake_head(s);
    Position new_head = head;
    
    switch (s->direction) {
        case DIR_UP:    new_head.y--; break;
        case DIR_DOWN:  new_head.y++; break;
        case DIR_LEFT:  new_head.x--; break;
        case DIR_RIGHT: new_head.x++; break;
    }
    
    s->head_idx = (s->head_idx + 1) % MAX_SNAKE_LEN;
    s->body[s->head_idx] = new_head;
}

static void rebuild_map(void) {
    /* Clear interior */
    for (int y = 1; y < GRID_SIZE - 1; y++) {
        for (int x = 1; x < GRID_SIZE - 1; x++) {
            g_state->map[y][x] = CELL_EMPTY;
        }
    }
    
    /* Place food */
    for (int i = 0; i < MAX_FOOD; i++) {
        if (g_state->foods[i].active) {
            int x = g_state->foods[i].pos.x;
            int y = g_state->foods[i].pos.y;
            if (x > 0 && x < GRID_SIZE - 1 && y > 0 && y < GRID_SIZE - 1) {
                g_state->map[y][x] = CELL_FOOD;
            }
        }
    }
    
    /* Place snakes */
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!g_state->players[p].active || !g_state->players[p].snake.alive)
            continue;
        
        Snake *s = &g_state->players[p].snake;
        
        for (int i = 0; i < s->length; i++) {
            int idx = (s->head_idx - s->length + 1 + i + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
            Position pos = s->body[idx];
            if (pos.x > 0 && pos.x < GRID_SIZE - 1 &&
                pos.y > 0 && pos.y < GRID_SIZE - 1) {
                g_state->map[pos.y][pos.x] = CELL_SNAKE_BASE + p;
            }
        }
    }
}

static void check_collisions(void) {
    for (int p = 0; p < MAX_PLAYERS; p++) {
        if (!g_state->players[p].active || !g_state->players[p].snake.alive)
            continue;
        
        /* Spawn protection */
        if (g_state->players[p].spawn_protection > 0) {
            g_state->players[p].spawn_protection--;
            continue;
        }
        
        Snake *s = &g_state->players[p].snake;
        Position head = snake_head(s);
        
        /* Wall collision */
        if (head.x <= 0 || head.x >= GRID_SIZE - 1 ||
            head.y <= 0 || head.y >= GRID_SIZE - 1) {
            s->alive = false;
            g_state->players[p].respawn_timer = RESPAWN_TICKS;
            printf("[GAME] %s hit wall! Respawning...\n", g_state->players[p].name);
            continue;
        }
        
        /* Food collision */
        for (int i = 0; i < MAX_FOOD; i++) {
            if (g_state->foods[i].active &&
                g_state->foods[i].pos.x == head.x &&
                g_state->foods[i].pos.y == head.y) {
                
                g_state->players[p].score += 10;
                if (s->length < MAX_SNAKE_LEN - 1) {
                    s->length++;
                }
                g_state->foods[i].active = false;
                g_state->food_count--;
                spawn_food();
                break;
            }
        }
        
        /* Snake collision */
        for (int other = 0; other < MAX_PLAYERS; other++) {
            if (!g_state->players[other].active || !g_state->players[other].snake.alive)
                continue;
            
            Snake *os = &g_state->players[other].snake;
            
            for (int i = 0; i < os->length; i++) {
                int idx = (os->head_idx - i + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
                
                /* Skip own head */
                if (p == other && i == 0) continue;
                
                if (os->body[idx].x == head.x && os->body[idx].y == head.y) {
                    s->alive = false;
                    g_state->players[p].respawn_timer = RESPAWN_TICKS;
                    printf("[GAME] %s collided! Respawning...\n", g_state->players[p].name);
                    break;
                }
            }
            if (!s->alive) break;
        }
    }
}

/* ============================================================================
 * Game Loop Process
 * ============================================================================ */

static void game_loop_process(void) {
    printf("[GAME] Game loop process started (PID: %d)\n", getpid());
    
    srand(time(NULL) ^ getpid());
    
    uint64_t last_tick = get_time_ms();
    uint64_t last_food_spawn = last_tick;
    
    while (g_state->running) {
        uint64_t now = get_time_ms();
        
        if (now - last_tick >= GAME_TICK_MS) {
            pthread_mutex_lock(&g_state->lock);
            
            /* Auto-respawn dead snakes */
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (g_state->players[p].active && !g_state->players[p].snake.alive) {
                    if (g_state->players[p].respawn_timer > 0) {
                        g_state->players[p].respawn_timer--;
                    } else {
                        int spawn_x, spawn_y;
                        find_spawn_pos(&spawn_x, &spawn_y);
                        init_snake(&g_state->players[p], spawn_x, spawn_y);
                        printf("[GAME] %s respawned!\n", g_state->players[p].name);
                        
                        char msg[64];
                        snprintf(msg, sizeof(msg), "%s respawned!", g_state->players[p].name);
                        add_chat_message(0, "SYSTEM", msg);
                    }
                }
            }
            
            /* Move all snakes */
            for (int p = 0; p < MAX_PLAYERS; p++) {
                if (g_state->players[p].active && g_state->players[p].snake.alive) {
                    move_snake(&g_state->players[p].snake);
                }
            }
            
            /* Check collisions */
            check_collisions();
            
            /* Rebuild map */
            rebuild_map();
            
            /* Spawn food periodically */
            if (now - last_food_spawn > 3000 && g_state->food_count < MAX_FOOD / 2) {
                spawn_food();
                last_food_spawn = now;
            }
            
            g_state->tick++;
            
            pthread_mutex_unlock(&g_state->lock);
            
            last_tick = now;
        }
        
        msleep(10);
    }
    
    printf("[GAME] Game loop process stopped.\n");
}

/* ============================================================================
 * Client Info (per worker)
 * ============================================================================ */

typedef struct {
    int fd;
    int player_slot;
    uint64_t last_chat_idx;
    uint64_t last_map_tick;
} ClientInfo;

/* ============================================================================
 * Handle Client Message
 * ============================================================================ */

static void handle_client_message(ClientInfo *client) {
    uint16_t opcode;
    void *payload = NULL;
    uint32_t len;
    
    if (recv_packet(client->fd, &opcode, &payload, &len) < 0) {
        /* Disconnect */
        if (client->player_slot >= 0) {
            pthread_mutex_lock(&g_state->lock);
            Player *p = &g_state->players[client->player_slot];
            printf("[SERVER] %s disconnected.\n", p->name);
            
            char msg[64];
            snprintf(msg, sizeof(msg), "%s left the game", p->name);
            add_chat_message(0, "SYSTEM", msg);
            
            p->active = false;
            p->snake.alive = false;
            g_state->player_count--;
            pthread_mutex_unlock(&g_state->lock);
        }
        close(client->fd);
        client->fd = -1;
        client->player_slot = -1;
        return;
    }
    
    switch (opcode) {
        case OP_LOGIN_REQ: {
            if (len < sizeof(LoginRequest)) break;
            LoginRequest *req = (LoginRequest *)payload;
            req->name[MAX_NAME_LEN - 1] = '\0';
            
            pthread_mutex_lock(&g_state->lock);
            
            int slot = -1;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (!g_state->players[i].active) {
                    slot = i;
                    break;
                }
            }
            
            if (slot < 0) {
                pthread_mutex_unlock(&g_state->lock);
                send_packet(client->fd, OP_ERROR, "Server Full", 11);
                break;
            }
            
            Player *p = &g_state->players[slot];
            memset(p, 0, sizeof(Player));
            p->id = g_state->next_player_id++;
            strncpy(p->name, req->name, MAX_NAME_LEN - 1);
            p->color = (slot % NUM_COLORS) + 1;
            p->active = true;
            p->is_ai = req->is_ai;
            
            int spawn_x, spawn_y;
            find_spawn_pos(&spawn_x, &spawn_y);
            init_snake(p, spawn_x, spawn_y);
            
            g_state->player_count++;
            client->player_slot = slot;
            client->last_chat_idx = g_state->chat_count;
            
            char join_msg[64];
            snprintf(join_msg, sizeof(join_msg), "%s joined!", p->name);
            add_chat_message(0, "SYSTEM", join_msg);
            
            pthread_mutex_unlock(&g_state->lock);
            
            LoginResponse resp = {
                .player_id = p->id,
                .color = p->color,
                .grid_width = GRID_SIZE,
                .grid_height = GRID_SIZE
            };
            send_packet(client->fd, OP_LOGIN_RESP, &resp, sizeof(resp));
            
            printf("[SERVER] %s joined (slot %d)\n", p->name, slot);
            break;
        }
        
        case OP_MOVE: {
            if (len < sizeof(MoveCommand)) break;
            MoveCommand *cmd = (MoveCommand *)payload;
            
            if (client->player_slot >= 0) {
                pthread_mutex_lock(&g_state->lock);
                Player *p = &g_state->players[client->player_slot];
                if (p->active && p->snake.alive && cmd->direction <= DIR_RIGHT) {
                    p->snake.pending_dir = cmd->direction;
                }
                pthread_mutex_unlock(&g_state->lock);
            }
            break;
        }
        
        case OP_CHAT_SEND: {
            if (len < 1) break;
            ChatSend *chat = (ChatSend *)payload;
            chat->text[MAX_CHAT_LEN - 1] = '\0';
            
            if (client->player_slot >= 0) {
                pthread_mutex_lock(&g_state->lock);
                Player *p = &g_state->players[client->player_slot];
                add_chat_message(p->id, p->name, chat->text);
                pthread_mutex_unlock(&g_state->lock);
            }
            break;
        }
        
        case OP_HEARTBEAT: {
            send_packet(client->fd, OP_HEARTBEAT_ACK, NULL, 0);
            break;
        }
        
        case OP_LOGOUT: {
            if (client->player_slot >= 0) {
                pthread_mutex_lock(&g_state->lock);
                Player *p = &g_state->players[client->player_slot];
                printf("[SERVER] %s logged out.\n", p->name);
                p->active = false;
                p->snake.alive = false;
                g_state->player_count--;
                pthread_mutex_unlock(&g_state->lock);
            }
            close(client->fd);
            client->fd = -1;
            client->player_slot = -1;
            break;
        }
    }
    
    if (payload) free(payload);
}

/* ============================================================================
 * Worker Process
 * ============================================================================ */

static void worker_process(int worker_id) {
    printf("[WORKER %d] Started (PID: %d)\n", worker_id, getpid());
    
    srand(time(NULL) ^ getpid());
    
    fd_set masterfds, readfds;
    int max_fd = g_server_fd;
    ClientInfo clients[FD_SETSIZE];
    
    for (int i = 0; i < FD_SETSIZE; i++) {
        clients[i].fd = -1;
        clients[i].player_slot = -1;
        clients[i].last_chat_idx = 0;
        clients[i].last_map_tick = 0;
    }
    
    FD_ZERO(&masterfds);
    FD_SET(g_server_fd, &masterfds);
    
    while (g_state->running) {
        readfds = masterfds;
        struct timeval tv = { 0, 50000 }; /* 50ms */
        
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        
        if (activity < 0 && errno != EINTR) {
            break;
        }
        
        uint64_t current_tick = g_state->tick;
        
        /* Send updates to all connected clients */
        for (int i = 0; i <= max_fd; i++) {
            if (i == g_server_fd || clients[i].fd <= 0 || clients[i].player_slot < 0)
                continue;
            
            /* Send map update if tick changed */
            if (clients[i].last_map_tick < current_tick) {
                MapUpdate update;
                
                pthread_mutex_lock(&g_state->lock);
                update.tick = g_state->tick;
                memcpy(update.map, g_state->map, sizeof(update.map));
                for (int j = 0; j < MAX_PLAYERS; j++) {
                    update.scores[j] = g_state->players[j].score;
                    update.alive[j] = g_state->players[j].snake.alive ? 1 : 0;
                    update.active[j] = g_state->players[j].active ? 1 : 0;
                    strncpy(update.names[j], g_state->players[j].name, MAX_NAME_LEN);
                }
                pthread_mutex_unlock(&g_state->lock);
                
                if (send_packet(clients[i].fd, OP_MAP_UPDATE, &update, sizeof(update)) == 0) {
                    clients[i].last_map_tick = current_tick;
                }
            }
            
            /* Send new chat messages */
            pthread_mutex_lock(&g_state->lock);
            uint64_t current_chat = g_state->chat_count;
            if (current_chat > clients[i].last_chat_idx) {
                uint64_t num_new = current_chat - clients[i].last_chat_idx;
                if (num_new > MAX_CHAT_HISTORY) num_new = MAX_CHAT_HISTORY;
                
                for (uint64_t c = 0; c < num_new; c++) {
                    uint64_t msg_num = clients[i].last_chat_idx + c;
                    int idx = msg_num % MAX_CHAT_HISTORY;
                    
                    ChatRecv chat_msg;
                    chat_msg.sender_id = g_state->chat_history[idx].sender_id;
                    strncpy(chat_msg.sender_name, g_state->chat_history[idx].sender_name, MAX_NAME_LEN);
                    strncpy(chat_msg.text, g_state->chat_history[idx].text, MAX_CHAT_LEN);
                    
                    send_packet(clients[i].fd, OP_CHAT_RECV, &chat_msg, sizeof(chat_msg));
                }
                clients[i].last_chat_idx = current_chat;
            }
            pthread_mutex_unlock(&g_state->lock);
        }
        
        if (activity > 0) {
            for (int i = 0; i <= max_fd; i++) {
                if (FD_ISSET(i, &readfds)) {
                    if (i == g_server_fd) {
                        /* New connection */
                        struct sockaddr_in client_addr;
                        socklen_t addr_len = sizeof(client_addr);
                        int new_fd = accept(g_server_fd, (struct sockaddr*)&client_addr, &addr_len);
                        
                        if (new_fd >= 0) {
                            FD_SET(new_fd, &masterfds);
                            if (new_fd > max_fd) max_fd = new_fd;
                            
                            clients[new_fd].fd = new_fd;
                            clients[new_fd].player_slot = -1;
                            clients[new_fd].last_chat_idx = g_state->chat_count;
                            clients[new_fd].last_map_tick = 0;
                            
                            char ip[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                            printf("[WORKER %d] New connection from %s (fd=%d)\n", 
                                   worker_id, ip, new_fd);
                        }
                    } else {
                        /* Client data */
                        handle_client_message(&clients[i]);
                        
                        if (clients[i].fd < 0) {
                            FD_CLR(i, &masterfds);
                        }
                    }
                }
            }
        }
    }
    
    printf("[WORKER %d] Stopped.\n", worker_id);
}

/* ============================================================================
 * Signal Handler
 * ============================================================================ */

static void signal_handler(int sig) {
    (void)sig;
    printf("\n[SERVER] Shutting down...\n");
    g_running = 0;
    if (g_state) g_state->running = 0;
}

/* ============================================================================
 * Cleanup
 * ============================================================================ */

static void cleanup(void) {
    printf("[SERVER] Cleaning up...\n");
    
    for (int i = 0; i < NUM_WORKERS; i++) {
        if (g_workers[i] > 0) {
            kill(g_workers[i], SIGTERM);
        }
    }
    
    if (g_game_loop_pid > 0) {
        kill(g_game_loop_pid, SIGTERM);
    }
    
    while (wait(NULL) > 0);
    
    if (g_state) {
        pthread_mutex_destroy(&g_state->lock);
        pthread_mutexattr_destroy(&g_state->lock_attr);
        shmdt(g_state);
    }
    
    if (g_shmid >= 0) {
        shmctl(g_shmid, IPC_RMID, NULL);
    }
    
    if (g_server_fd >= 0) {
        close(g_server_fd);
    }
    
    printf("[SERVER] Cleanup complete.\n");
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    int port = SERVER_PORT;
    
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    srand(time(NULL));
    
    /* Create shared memory */
    key_t key = ftok(SHM_KEY_FILE, SHM_KEY_ID);
    if (key == -1) {
        perror("ftok");
        return 1;
    }
    
    g_shmid = shmget(key, sizeof(GameState), IPC_CREAT | 0666);
    if (g_shmid < 0) {
        perror("shmget");
        return 1;
    }
    
    g_state = (GameState *)shmat(g_shmid, NULL, 0);
    if (g_state == (void *)-1) {
        perror("shmat");
        return 1;
    }
    
    /* Initialize game state */
    init_game_state();
    
    /* Create server socket */
    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("socket");
        cleanup();
        return 1;
    }
    
    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    
    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        cleanup();
        return 1;
    }
    
    if (listen(g_server_fd, 128) < 0) {
        perror("listen");
        cleanup();
        return 1;
    }
    
    printf("================================================\n");
    printf("  Snake Game + Chatroom Server\n");
    printf("  (Multi-Process + Shared Memory IPC)\n");
    printf("================================================\n");
    printf("  Port:        %d\n", port);
    printf("  Grid:        %dx%d\n", GRID_SIZE, GRID_SIZE);
    printf("  Max Players: %d\n", MAX_PLAYERS);
    printf("  Workers:     %d (prefork)\n", NUM_WORKERS);
    printf("  IPC:         System V Shared Memory\n");
    printf("  SHM ID:      %d\n", g_shmid);
    printf("================================================\n");
    fflush(stdout);
    
    /* Fork game loop process */
    pid_t pid = fork();
    if (pid == 0) {
        game_loop_process();
        exit(0);
    } else if (pid > 0) {
        g_game_loop_pid = pid;
    } else {
        perror("fork game loop");
        cleanup();
        return 1;
    }
    
    /* Prefork workers */
    for (int i = 0; i < NUM_WORKERS; i++) {
        pid = fork();
        if (pid == 0) {
            worker_process(i);
            exit(0);
        } else if (pid > 0) {
            g_workers[i] = pid;
        } else {
            perror("fork worker");
            cleanup();
            return 1;
        }
    }
    
    printf("[SERVER] All processes started. Press Ctrl+C to stop.\n");
    
    while (g_running) {
        pause();
    }
    
    cleanup();
    return 0;
}
