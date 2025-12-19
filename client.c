/**
 * client.c - Multi-Threaded Snake Game + Chatroom Client
 * 
 * Features:
 * - ncurses UI with game board and chat window
 * - Customizable key bindings
 * - Real-time chat
 * - Stress test mode (100 AI clients)
 * 
 * Threads:
 * - Main thread: ncurses rendering, keyboard input
 * - Receiver thread: receive packets from server
 * - Heartbeat thread: keep-alive
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ncurses.h>
#include <errno.h>

#include "common.h"
#include "proto.h"

/* ============================================================================
 * Global State
 * ============================================================================ */

static volatile int g_running = 1;
static volatile int g_connected = 0;
static int g_socket_fd = -1;
static uint32_t g_my_id = 0;
static int g_my_slot = -1;
static char g_my_name[MAX_NAME_LEN] = "Player";
static uint8_t g_my_color = 1;

/* Map state (received from server) */
static MapUpdate g_map_state;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

/* Chat state */
static ChatRecv g_chat_messages[MAX_CHAT_HISTORY];
static int g_chat_count = 0;
static pthread_mutex_t g_chat_lock = PTHREAD_MUTEX_INITIALIZER;

/* Chat input mode */
static int g_chat_mode = 0;
static char g_chat_input[MAX_CHAT_LEN] = {0};
static int g_chat_input_len = 0;

/* Key bindings */
static int g_keys[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };

/* ncurses windows */
static WINDOW *g_game_win = NULL;
static WINDOW *g_chat_win = NULL;
static WINDOW *g_status_win = NULL;
static WINDOW *g_input_win = NULL;
static WINDOW *g_score_win = NULL;

/* Stress test mode */
static int g_stress_mode = 0;
static long g_total_rtt = 0;
static long g_total_requests = 0;
static long g_successful_connections = 0;
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================================
 * Utility
 * ============================================================================ */

static void msleep(int ms) {
    usleep(ms * 1000);
}

/* ============================================================================
 * Network
 * ============================================================================ */

static int connect_to_server(const char *host, int port) {
    struct hostent *he = gethostbyname(host);
    if (!he) {
        fprintf(stderr, "Cannot resolve host: %s\n", host);
        return -1;
    }
    
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port)
    };
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    
    return fd;
}

static int do_login(int fd, const char *name, bool is_ai) {
    LoginRequest req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, name, MAX_NAME_LEN - 1);
    req.is_ai = is_ai;
    
    if (send_packet(fd, OP_LOGIN_REQ, &req, sizeof(req)) < 0) {
        return -1;
    }
    
    uint16_t opcode;
    void *payload = NULL;
    uint32_t len;
    
    if (recv_packet(fd, &opcode, &payload, &len) < 0) {
        return -1;
    }
    
    if (opcode == OP_ERROR) {
        fprintf(stderr, "Login error: %s\n", (char *)payload);
        free(payload);
        return -1;
    }
    
    if (opcode != OP_LOGIN_RESP || len < sizeof(LoginResponse)) {
        free(payload);
        return -1;
    }
    
    LoginResponse *resp = (LoginResponse *)payload;
    g_my_id = resp->player_id;
    g_my_color = resp->color;
    
    free(payload);
    return 0;
}

static void send_move(uint8_t direction) {
    MoveCommand cmd = { .direction = direction };
    send_packet(g_socket_fd, OP_MOVE, &cmd, sizeof(cmd));
}

static void send_chat(const char *text) {
    ChatSend chat;
    memset(&chat, 0, sizeof(chat));
    strncpy(chat.text, text, MAX_CHAT_LEN - 1);
    send_packet(g_socket_fd, OP_CHAT_SEND, &chat, sizeof(chat));
}

/* ============================================================================
 * Receiver Thread
 * ============================================================================ */

static void *receiver_thread(void *arg) {
    (void)arg;
    
    while (g_running && g_connected) {
        uint16_t opcode;
        void *payload = NULL;
        uint32_t len;
        
        if (recv_packet(g_socket_fd, &opcode, &payload, &len) < 0) {
            g_connected = 0;
            break;
        }
        
        switch (opcode) {
            case OP_MAP_UPDATE: {
                if (len >= sizeof(MapUpdate)) {
                    pthread_mutex_lock(&g_map_lock);
                    memcpy(&g_map_state, payload, sizeof(MapUpdate));
                    
                    /* Find my slot by name */
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        if (g_map_state.active[i] && 
                            strncmp(g_map_state.names[i], g_my_name, MAX_NAME_LEN) == 0) {
                            g_my_slot = i;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&g_map_lock);
                }
                break;
            }
            
            case OP_CHAT_RECV: {
                if (len >= sizeof(ChatRecv)) {
                    pthread_mutex_lock(&g_chat_lock);
                    if (g_chat_count < MAX_CHAT_HISTORY) {
                        memcpy(&g_chat_messages[g_chat_count], payload, sizeof(ChatRecv));
                        g_chat_count++;
                    } else {
                        memmove(&g_chat_messages[0], &g_chat_messages[1], 
                                sizeof(ChatRecv) * (MAX_CHAT_HISTORY - 1));
                        memcpy(&g_chat_messages[MAX_CHAT_HISTORY - 1], payload, sizeof(ChatRecv));
                    }
                    pthread_mutex_unlock(&g_chat_lock);
                }
                break;
            }
            
            case OP_PLAYER_DIE: {
                /* Could show death message */
                break;
            }
            
            case OP_HEARTBEAT_ACK: {
                /* Connection is alive */
                break;
            }
            
            default:
                break;
        }
        
        if (payload) free(payload);
    }
    
    return NULL;
}

/* ============================================================================
 * Heartbeat Thread
 * ============================================================================ */

static void *heartbeat_thread(void *arg) {
    (void)arg;
    
    while (g_running && g_connected) {
        sleep(3);
        if (g_running && g_connected) {
            send_packet(g_socket_fd, OP_HEARTBEAT, NULL, 0);
        }
    }
    
    return NULL;
}

/* ============================================================================
 * ncurses UI
 * ============================================================================ */

static void init_colors(void) {
    start_color();
    use_default_colors();
    
    init_pair(1, COLOR_GREEN, -1);
    init_pair(2, COLOR_BLUE, -1);
    init_pair(3, COLOR_MAGENTA, -1);
    init_pair(4, COLOR_YELLOW, -1);
    init_pair(5, COLOR_CYAN, -1);
    init_pair(6, COLOR_RED, -1);
    init_pair(7, COLOR_WHITE, -1);
    init_pair(8, COLOR_RED, -1);     /* Food */
    init_pair(9, COLOR_WHITE, -1);   /* Wall */
    init_pair(10, COLOR_GREEN, -1);  /* System messages */
}

static void init_ui(void) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    
    if (has_colors()) {
        init_colors();
    }
    
    int game_h = GRID_SIZE + 2;
    int game_w = GRID_SIZE + 2;
    int chat_w = 35;
    int score_h = 15;
    
    /* Game window (left) */
    g_game_win = newwin(game_h, game_w, 0, 0);
    
    /* Score window (top right) */
    g_score_win = newwin(score_h, chat_w, 0, game_w + 1);
    
    /* Chat window (middle right) */
    g_chat_win = newwin(game_h - score_h - 5, chat_w, score_h, game_w + 1);
    
    /* Status window (bottom) */
    g_status_win = newwin(2, game_w + chat_w + 1, game_h, 0);
    
    /* Input window (bottom) */
    g_input_win = newwin(2, game_w + chat_w + 1, game_h + 2, 0);
    
    refresh();
}

static void shutdown_ui(void) {
    if (g_game_win) delwin(g_game_win);
    if (g_chat_win) delwin(g_chat_win);
    if (g_status_win) delwin(g_status_win);
    if (g_input_win) delwin(g_input_win);
    if (g_score_win) delwin(g_score_win);
    endwin();
}

static void draw_game(void) {
    werase(g_game_win);
    box(g_game_win, 0, 0);
    
    wattron(g_game_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(g_game_win, 0, 2, " Snake Game ");
    wattroff(g_game_win, COLOR_PAIR(1) | A_BOLD);
    
    pthread_mutex_lock(&g_map_lock);
    
    for (int y = 0; y < GRID_SIZE; y++) {
        for (int x = 0; x < GRID_SIZE; x++) {
            uint8_t cell = g_map_state.map[y][x];
            int screen_y = y + 1;
            int screen_x = x + 1;
            
            if (cell == CELL_WALL) {
                wattron(g_game_win, COLOR_PAIR(9));
                mvwaddch(g_game_win, screen_y, screen_x, '#');
                wattroff(g_game_win, COLOR_PAIR(9));
            }
            else if (cell == CELL_FOOD) {
                wattron(g_game_win, COLOR_PAIR(8) | A_BOLD);
                mvwaddch(g_game_win, screen_y, screen_x, '@');
                wattroff(g_game_win, COLOR_PAIR(8) | A_BOLD);
            }
            else if (cell >= CELL_SNAKE_BASE) {
                int player_idx = cell - CELL_SNAKE_BASE;
                int color = (player_idx % NUM_COLORS) + 1;
                
                /* Bold for my snake */
                if (player_idx == g_my_slot) {
                    wattron(g_game_win, A_BOLD);
                }
                
                wattron(g_game_win, COLOR_PAIR(color));
                mvwaddch(g_game_win, screen_y, screen_x, 'O');
                wattroff(g_game_win, COLOR_PAIR(color));
                wattroff(g_game_win, A_BOLD);
            }
        }
    }
    
    pthread_mutex_unlock(&g_map_lock);
    
    wrefresh(g_game_win);
}

static void draw_scores(void) {
    werase(g_score_win);
    box(g_score_win, 0, 0);
    
    wattron(g_score_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(g_score_win, 0, 2, " Scoreboard ");
    wattroff(g_score_win, COLOR_PAIR(1) | A_BOLD);
    
    pthread_mutex_lock(&g_map_lock);
    
    int row = 1;
    for (int i = 0; i < MAX_PLAYERS && row < 13; i++) {
        if (g_map_state.active[i]) {
            int color = (i % NUM_COLORS) + 1;
            char status = g_map_state.alive[i] ? 'O' : '.';  /* O=alive, .=respawning */
            
            if (i == g_my_slot) {
                wattron(g_score_win, A_BOLD);
            }
            
            wattron(g_score_win, COLOR_PAIR(color));
            mvwprintw(g_score_win, row, 2, "%c %-12.12s %5d",
                     status, g_map_state.names[i], g_map_state.scores[i]);
            wattroff(g_score_win, COLOR_PAIR(color));
            wattroff(g_score_win, A_BOLD);
            
            row++;
        }
    }
    
    mvwprintw(g_score_win, row + 1, 2, "Tick: %u", g_map_state.tick);
    
    pthread_mutex_unlock(&g_map_lock);
    
    wrefresh(g_score_win);
}

static void draw_chat(void) {
    werase(g_chat_win);
    box(g_chat_win, 0, 0);
    
    wattron(g_chat_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(g_chat_win, 0, 2, " Chat ");
    wattroff(g_chat_win, COLOR_PAIR(1) | A_BOLD);
    
    pthread_mutex_lock(&g_chat_lock);
    
    int max_lines = GRID_SIZE - 15 - 7;
    int start = g_chat_count > max_lines ? g_chat_count - max_lines : 0;
    int row = 1;
    
    for (int i = start; i < g_chat_count && row < max_lines + 1; i++) {
        int color = 7; /* Default white */
        if (strcmp(g_chat_messages[i].sender_name, "SYSTEM") == 0) {
            color = 10;
        }
        
        wattron(g_chat_win, COLOR_PAIR(color));
        mvwprintw(g_chat_win, row, 1, "%-8.8s: %-22.22s",
                  g_chat_messages[i].sender_name,
                  g_chat_messages[i].text);
        wattroff(g_chat_win, COLOR_PAIR(color));
        row++;
    }
    
    pthread_mutex_unlock(&g_chat_lock);
    
    wrefresh(g_chat_win);
}

static void draw_status(void) {
    werase(g_status_win);
    
    if (g_chat_mode) {
        wattron(g_status_win, COLOR_PAIR(4));
        mvwprintw(g_status_win, 0, 2, "[CHAT MODE] Type message, Enter=Send, Esc=Cancel");
        wattroff(g_status_win, COLOR_PAIR(4));
    } else {
        mvwprintw(g_status_win, 0, 2, "Controls: Arrow keys=Move | Tab=Chat | Q=Quit");
    }
    
    mvwprintw(g_status_win, 1, 2, "Player: %s | Connected: %s", 
             g_my_name, g_connected ? "YES" : "NO");
    
    wrefresh(g_status_win);
}

static void draw_input(void) {
    werase(g_input_win);
    
    if (g_chat_mode) {
        wattron(g_input_win, COLOR_PAIR(4));
        mvwprintw(g_input_win, 0, 0, "Chat: %s_", g_chat_input);
        wattroff(g_input_win, COLOR_PAIR(4));
    }
    
    wrefresh(g_input_win);
}

/* ============================================================================
 * Key Binding Setup
 * ============================================================================ */

static void setup_key_bindings(void) {
    printf("=== Key Binding Setup ===\n");
    printf("Press 4 keys for: UP, DOWN, LEFT, RIGHT\n");
    printf("(Press Enter to use arrow keys as default)\n\n");
    
    char buf[16];
    printf("UP key (or Enter for ArrowUp): ");
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
        g_keys[0] = buf[0];
    }
    
    printf("DOWN key (or Enter for ArrowDown): ");
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
        g_keys[1] = buf[0];
    }
    
    printf("LEFT key (or Enter for ArrowLeft): ");
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
        g_keys[2] = buf[0];
    }
    
    printf("RIGHT key (or Enter for ArrowRight): ");
    fflush(stdout);
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n') {
        g_keys[3] = buf[0];
    }
    
    printf("\nKey bindings set!\n");
    printf("UP=%c DOWN=%c LEFT=%c RIGHT=%c\n",
           g_keys[0] < 256 ? g_keys[0] : '^',
           g_keys[1] < 256 ? g_keys[1] : 'v',
           g_keys[2] < 256 ? g_keys[2] : '<',
           g_keys[3] < 256 ? g_keys[3] : '>');
}

/* ============================================================================
 * Signal Handler
 * ============================================================================ */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ============================================================================
 * Stress Test Mode
 * ============================================================================ */

static void *stress_client_thread(void *arg) {
    int thread_id = (int)(intptr_t)arg;
    
    int sock = connect_to_server("127.0.0.1", SERVER_PORT);
    if (sock < 0) {
        return NULL;
    }
    
    pthread_mutex_lock(&g_stats_lock);
    g_successful_connections++;
    pthread_mutex_unlock(&g_stats_lock);
    
    /* Login as AI */
    char name[MAX_NAME_LEN];
    snprintf(name, sizeof(name), "AI_%03d", thread_id);
    
    LoginRequest req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, name, MAX_NAME_LEN - 1);
    req.is_ai = true;
    
    if (send_packet(sock, OP_LOGIN_REQ, &req, sizeof(req)) < 0) {
        close(sock);
        return NULL;
    }
    
    uint16_t opcode;
    void *payload = NULL;
    uint32_t len;
    
    if (recv_packet(sock, &opcode, &payload, &len) < 0) {
        close(sock);
        return NULL;
    }
    free(payload);
    
    /* Send random moves for a while */
    uint8_t dirs[] = { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
    struct timeval start, end;
    
    for (int i = 0; i < 50 && g_running; i++) {
        MoveCommand cmd = { .direction = dirs[rand() % 4] };
        
        gettimeofday(&start, NULL);
        
        if (send_packet(sock, OP_MOVE, &cmd, sizeof(cmd)) < 0) {
            break;
        }
        
        /* Wait for map update */
        payload = NULL;
        if (recv_packet_timeout(sock, &opcode, &payload, &len, 500) < 0) {
            if (payload) free(payload);
            break;
        }
        
        gettimeofday(&end, NULL);
        
        long rtt = (end.tv_sec - start.tv_sec) * 1000000 + 
                   (end.tv_usec - start.tv_usec);
        
        pthread_mutex_lock(&g_stats_lock);
        g_total_rtt += rtt;
        g_total_requests++;
        pthread_mutex_unlock(&g_stats_lock);
        
        if (payload) free(payload);
        
        msleep(100);
    }
    
    close(sock);
    return NULL;
}

static void run_stress_test(int num_clients) {
    printf("========================================\n");
    printf("  Stress Test - %d Concurrent Clients\n", num_clients);
    printf("========================================\n");
    
    struct timeval start_time, end_time;
    gettimeofday(&start_time, NULL);
    
    pthread_t *threads = malloc(sizeof(pthread_t) * num_clients);
    
    for (int i = 0; i < num_clients; i++) {
        pthread_create(&threads[i], NULL, stress_client_thread, (void *)(intptr_t)i);
        msleep(20);
    }
    
    for (int i = 0; i < num_clients; i++) {
        pthread_join(threads[i], NULL);
    }
    
    free(threads);
    
    gettimeofday(&end_time, NULL);
    
    double elapsed = (end_time.tv_sec - start_time.tv_sec) +
                    (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    
    printf("\n========================================\n");
    printf("  Stress Test Results\n");
    printf("========================================\n");
    printf("  Clients:      %d\n", num_clients);
    printf("  Connected:    %ld\n", g_successful_connections);
    printf("  Requests:     %ld\n", g_total_requests);
    printf("  Time:         %.2f sec\n", elapsed);
    
    if (g_total_requests > 0) {
        printf("  Avg Latency:  %ld us (%.2f ms)\n",
               g_total_rtt / g_total_requests,
               (double)(g_total_rtt / g_total_requests) / 1000.0);
        printf("  Throughput:   %.2f req/sec\n",
               (double)g_total_requests / elapsed);
    }
    printf("========================================\n");
}

/* ============================================================================
 * Main
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -h HOST     Server hostname (default: 127.0.0.1)\n");
    printf("  -p PORT     Server port (default: %d)\n", SERVER_PORT);
    printf("  -n NAME     Player name (default: Player)\n");
    printf("  -s [N]      Stress test with N clients (default: 100)\n");
    printf("  --help      Show this help\n");
}

int main(int argc, char *argv[]) {
    char *host = "127.0.0.1";
    int port = SERVER_PORT;
    int stress_clients = 100;
    
    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            strncpy(g_my_name, argv[++i], MAX_NAME_LEN - 1);
        } else if (strcmp(argv[i], "-s") == 0) {
            g_stress_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                stress_clients = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    /* Signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    
    srand(time(NULL));
    
    /* Stress test mode */
    if (g_stress_mode) {
        run_stress_test(stress_clients);
        return 0;
    }
    
    /* Interactive mode - setup key bindings */
    setup_key_bindings();
    
    /* Connect to server */
    printf("\nConnecting to %s:%d...\n", host, port);
    g_socket_fd = connect_to_server(host, port);
    if (g_socket_fd < 0) {
        return 1;
    }
    
    printf("Connected! Logging in as '%s'...\n", g_my_name);
    if (do_login(g_socket_fd, g_my_name, false) < 0) {
        close(g_socket_fd);
        return 1;
    }
    
    printf("Logged in! Starting game...\n");
    sleep(1);
    
    g_connected = 1;
    
    /* Start threads */
    pthread_t recv_thread, hb_thread;
    pthread_create(&recv_thread, NULL, receiver_thread, NULL);
    pthread_create(&hb_thread, NULL, heartbeat_thread, NULL);
    
    /* Initialize ncurses */
    init_ui();
    
    /* Main loop */
    while (g_running && g_connected) {
        int ch = getch();
        
        if (g_chat_mode) {
            /* Chat input mode */
            if (ch == '\n' || ch == KEY_ENTER) {
                if (g_chat_input_len > 0) {
                    send_chat(g_chat_input);
                }
                g_chat_mode = 0;
                g_chat_input[0] = '\0';
                g_chat_input_len = 0;
            } else if (ch == 27) { /* Escape */
                g_chat_mode = 0;
                g_chat_input[0] = '\0';
                g_chat_input_len = 0;
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (g_chat_input_len > 0) {
                    g_chat_input[--g_chat_input_len] = '\0';
                }
            } else if (ch >= 32 && ch < 127 && g_chat_input_len < MAX_CHAT_LEN - 1) {
                g_chat_input[g_chat_input_len++] = ch;
                g_chat_input[g_chat_input_len] = '\0';
            }
        } else {
            /* Game mode */
            if (ch == g_keys[0] || ch == KEY_UP) {
                send_move(DIR_UP);
            } else if (ch == g_keys[1] || ch == KEY_DOWN) {
                send_move(DIR_DOWN);
            } else if (ch == g_keys[2] || ch == KEY_LEFT) {
                send_move(DIR_LEFT);
            } else if (ch == g_keys[3] || ch == KEY_RIGHT) {
                send_move(DIR_RIGHT);
            } else if (ch == '\t') {
                g_chat_mode = 1;
            } else if (ch == 'q' || ch == 'Q') {
                g_running = 0;
            }
        }
        
        /* Draw UI */
        draw_game();
        draw_scores();
        draw_chat();
        draw_status();
        draw_input();
        
        msleep(16); /* ~60 FPS */
    }
    
    /* Cleanup */
    shutdown_ui();
    
    g_connected = 0;
    g_running = 0;
    
    pthread_join(recv_thread, NULL);
    pthread_join(hb_thread, NULL);
    
    close(g_socket_fd);
    
    printf("Goodbye!\n");
    return 0;
}
