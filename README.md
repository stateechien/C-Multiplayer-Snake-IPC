#  Snake Game + Chatroom - Network and System Programming & OS Final Project

多人貪食蛇遊戲 + 即時聊天室，符合作業系統期末專案所有要求。

## 課程要求 Checklist

| 要求 | 狀態 | 實作方式 |
|------|------|----------|
| **Server: Multi-process** | ok | Prefork Workers + Game Loop Process |
| **IPC: 共享記憶體** | ok | System V Shared Memory (shmget/shmat) |
| **Client: Multi-threaded** | ok | Input + Receiver + Heartbeat 三個執行緒 |
| **100+ 併發壓力測試** | ok | `./client -s 100` |
| **延遲/吞吐量統計** | ok | Avg Latency (μs) + Throughput (req/sec) |
| **Custom Protocol** | ok | 8-byte header [Len\|OpCode\|Checksum] |
| **Security: Checksum** | ok | 16-bit checksum 驗證 |
| **Security: 加密** | ok | XOR cipher (key: 0x5A) |
| **Reliability: Heartbeat** | ok | Client 每 3 秒發送心跳 |
| **Reliability: Timeout** | ok | Server 追蹤連線狀態 |
| **Modularity: 靜態函式庫** | ok | libproto.a |
| **README 文件** | ok | 本文件 |

## 系統架構

### Server 架構 (Multi-Process + Shared Memory)

```
                    ┌─────────────────────────────────────┐
                    │         Shared Memory (IPC)         │
                    │  ┌─────────────────────────────────┐│
                    │  │ GameState                       ││
                    │  │ ├── map[50][50]                 ││
                    │  │ ├── players[100]                ││
                    │  │ ├── foods[20]                   ││
                    │  │ ├── chat_history[50]            ││
                    │  │ └── pthread_mutex (PROCESS_SHARED)│
                    │  └─────────────────────────────────┘│
                    └─────────────────────────────────────┘
                              ▲           ▲
                              │           │
              ┌───────────────┴───┐   ┌───┴───────────────┐
              │                   │   │                   │
    ┌─────────┴─────────┐ ┌───────┴───────┐ ┌─────────────┴─────────┐
    │  Game Loop Process│ │ Worker Process │ │ Worker Process × N   │
    │  (fork)           │ │ (prefork)      │ │ (prefork)            │
    │                   │ │                │ │                      │
    │ • Move snakes     │ │ • Accept conn  │ │ • Handle clients     │
    │ • Check collision │ │ • Recv commands│ │ • Send map updates   │
    │ • Auto respawn    │ │ • Send updates │ │ • Broadcast chat     │
    │ • Rebuild map     │ │ • Chat relay   │ │                      │
    └───────────────────┘ └────────────────┘ └──────────────────────┘
```

### Client 架構 (Multi-Threaded)

```
    ┌─────────────────────────────────────────────────────┐
    │                    Client Process                    │
    │  ┌─────────────┐ ┌─────────────┐ ┌───────────────┐  │
    │  │ Main Thread │ │ Recv Thread │ │Heartbeat Thread│ │
    │  │             │ │             │ │               │  │
    │  │ • ncurses UI│ │ • Map update│ │ • Keep-alive  │  │
    │  │ • Key input │ │ • Chat recv │ │ • Every 3 sec │  │
    │  │ • Send move │ │ • Scoreboard│ │               │  │
    │  └─────────────┘ └─────────────┘ └───────────────┘  │
    └─────────────────────────────────────────────────────┘
```

## Protocol 規格

### Packet Header (8 bytes)

```
┌────────────┬────────────┬────────────┐
│  Length    │  OpCode    │  Checksum  │
│  (4 bytes) │  (2 bytes) │  (2 bytes) │
└────────────┴────────────┴────────────┘
```

### OpCodes

| OpCode | 名稱 | 方向 | 說明 |
|--------|------|------|------|
| 0x0001 | LOGIN_REQ | C→S | 登入請求 |
| 0x0002 | LOGIN_RESP | S→C | 登入回應 |
| 0x0003 | MOVE | C→S | 移動方向 |
| 0x0004 | MAP_UPDATE | S→C | 地圖更新 |
| 0x0005 | CHAT_SEND | C→S | 發送聊天 |
| 0x0006 | CHAT_RECV | S→C | 接收聊天 |
| 0x0010 | HEARTBEAT | C→S | 心跳包 |
| 0x0011 | HEARTBEAT_ACK | S→C | 心跳回應 |

### 安全機制

1. **Checksum**: 計算 payload 所有 bytes 的總和 (16-bit)
2. **XOR 加密**: 所有 payload 使用 key=0x5A 進行 XOR

```
發送流程: Raw Data → Calculate Checksum → XOR Encrypt → Send
接收流程: Receive → XOR Decrypt → Verify Checksum → Process
```

## 編譯與執行

### 編譯

```bash
# 安裝依賴 (Ubuntu/WSL)
sudo apt install build-essential libncurses-dev

# 編譯
make clean && make all
```

### 執行

```bash
# Terminal 1: 啟動 Server
./server

# Terminal 2: 玩家 1
./client -n Alice

# Terminal 3: 玩家 2
./client -n Bob

# 壓力測試 (100 個 AI clients)
./client -s 100
```

### 如果 Server 崩潰

```bash
# 清理共享記憶體
make clean-shm

# 或手動清理
ipcrm -M 0x5e5e5e5e
```

## 遊戲操作

| 按鍵 | 功能 |
|------|------|
| ↑ ↓ ← → | 移動蛇 |
| Tab | 進入聊天模式 |
| Enter | 發送訊息 |
| Esc | 取消聊天 |
| Q | 離開遊戲 |

## 遊戲特色

- **多人連線**: 最多 100 位玩家同時遊戲
- **即時聊天**: 所有玩家共享聊天室
- **自動復活**: 死亡後 3 秒自動重生
- **出生保護**: 重生後 3 秒無敵
- **吃食物長大**: 每吃一個食物 +10 分

## 壓力測試輸出範例

```
========================================
  Stress Test - 100 Concurrent Clients
========================================
  Clients:      100
  Connected:    100
  Requests:     5000
  Time:         12.34 sec
  Avg Latency:  2500 us (2.50 ms)
  Throughput:   405.19 req/sec
========================================
```

## 檔案結構

```
SnakeFinal/
├── common.h      # 共用定義 (常數、結構、協定)
├── proto.h       # Protocol 函式宣告
├── proto.c       # Protocol 實作 (checksum + XOR)
├── server.c      # Multi-process Server
├── client.c      # Multi-threaded Client
├── Makefile      # 編譯腳本
├── README.md     # 本文件
└── libproto.a    # 靜態函式庫 (編譯產生)
```

## 團隊分工

| 成員 | 負責模組 |
|------|----------|
| A | Server (Multi-process, Shared Memory, Game Loop) |
| B | Client (Multi-thread, ncurses UI, 壓力測試) |
| C | Protocol (Checksum, XOR 加密, 封包處理) |
| D | 整合測試、文件撰寫、Bug 修復 |

## 技術細節

### 為什麼選擇 Prefork + Shared Memory?

1. **Prefork (預先 fork)**
   - 避免 accept 時才 fork 的延遲
   - Worker 進程重複使用，效率高
   - 符合課程「Multi-process」要求

2. **System V Shared Memory**
   - 所有 Worker 共享同一份遊戲狀態
   - 使用 `PTHREAD_PROCESS_SHARED` mutex 同步
   - 符合課程「IPC」要求

3. **獨立 Game Loop Process**
   - 固定 100ms tick rate 更新遊戲
   - 不受 client I/O 影響
   - 確保遊戲邏輯一致性

### 同步機制

```c
// Shared Memory 中的 mutex
pthread_mutexattr_t attr;
pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
pthread_mutex_init(&g_state->lock, &attr);

// 存取共享狀態時加鎖
pthread_mutex_lock(&g_state->lock);
// ... 讀寫 game state ...
pthread_mutex_unlock(&g_state->lock);
```

## License

MIT License
