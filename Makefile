# ============================================================================
# Snake Game + Chatroom - Final Project
# Multi-Process Server + Shared Memory IPC
# ============================================================================

CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS_SERVER = -lpthread -lrt
LDFLAGS_CLIENT = -lpthread -lncurses

# Library
LIB_SRCS = proto.c
LIB_OBJS = $(LIB_SRCS:.c=.o)

# Targets
all: libproto.a server client
	@echo ""
	@echo "Build complete!"
	@echo "  ./server         - Start server"
	@echo "  ./client -n NAME - Start client"
	@echo "  ./client -s 100  - Stress test"

libproto.a: $(LIB_OBJS)
	ar rcs $@ $^
	@echo "Built: libproto.a (static library)"

proto.o: proto.c proto.h common.h
	$(CC) $(CFLAGS) -c proto.c

server: server.c libproto.a common.h proto.h
	$(CC) $(CFLAGS) -o $@ server.c -L. -lproto $(LDFLAGS_SERVER)
	@echo "Built: server (multi-process + shared memory)"

client: client.c libproto.a common.h proto.h
	$(CC) $(CFLAGS) -o $@ client.c -L. -lproto $(LDFLAGS_CLIENT)
	@echo "Built: client (multi-threaded + ncurses)"

# Stress test
stress: client server
	@echo "Starting stress test with 100 clients..."
	./client -s 100

# Clean shared memory (if server crashed)
clean-shm:
	@echo "Cleaning shared memory..."
	-ipcrm -M 0x5e5e5e5e 2>/dev/null || true
	-ipcs -m | grep $(USER) | awk '{print $$2}' | xargs -I {} ipcrm -m {} 2>/dev/null || true
	@echo "Done"

# Clean build
clean:
	rm -f *.o *.a server client

# Help
help:
	@echo "=============================================="
	@echo "  Snake Game + Chatroom - Final Project"
	@echo "=============================================="
	@echo ""
	@echo "Build:"
	@echo "  make all       - Build everything"
	@echo "  make clean     - Remove build files"
	@echo "  make clean-shm - Clean shared memory"
	@echo ""
	@echo "Run:"
	@echo "  ./server [port]         - Start server"
	@echo "  ./client -n NAME        - Game mode"
	@echo "  ./client -s [N]         - Stress test"
	@echo ""
	@echo "Architecture:"
	@echo "  Server: Multi-process (prefork + game loop)"
	@echo "  Client: Multi-threaded (input + recv + heartbeat)"
	@echo "  IPC:    System V Shared Memory"
	@echo ""

.PHONY: all clean clean-shm stress help
