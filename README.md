# 🚀 KTP: Reliable Transfer Protocol over UDP

[![Language: C](https://img.shields.io/badge/Language-C-00599C?logo=c&logoColor=white)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Platform: POSIX / Linux](https://img.shields.io/badge/Platform-POSIX%20%2F%20Linux-FCC624?logo=linux&logoColor=black)](https://www.kernel.org/)
[![Networking: UDP](https://img.shields.io/badge/Networking-UDP%20%2F%20Sockets-2496ED)](https://en.wikipedia.org/wiki/User_Datagram_Protocol)
[![Protocol: Sliding Window](https://img.shields.io/badge/Protocol-Sliding%20Window%20%2F%20ARQ-brightgreen)](https://en.wikipedia.org/wiki/Sliding_window_protocol)

**KTP (Kernel-level/Custom Transport Protocol)** is a robust, reliable data transfer protocol engineered on top of unreliable UDP datagram sockets. It implements connection-oriented reliable transport features including **sliding window flow control**, **selective retransmission with wall-clock timeouts**, **in-order packet delivery**, **loss simulation**, and **graceful connection teardown (FIN/FAK)**.

---

## 📑 Table of Contents

- [Overview](#-overview)
- [Architecture & Design](#-architecture--design)
  - [Shared Memory IPC](#1-shared-memory-ipc)
  - [Concurrency & Synchronization](#2-concurrency--synchronization)
  - [The `initk` Multi-Threaded Engine](#3-the-initk-multi-threaded-engine)
- [Packet Format](#-packet-format)
- [API Reference](#-api-reference)
- [Sliding Window & Flow Control](#-sliding-window--flow-control)
- [Loss Simulation & Empirical Performance](#-loss-simulation--empirical-performance)
- [Project Structure](#-project-structure)
- [Build & Compilation](#-build--compilation)
- [Usage Guide & Demo](#-usage-guide--demo)
  - [1. Launch the Protocol Engine](#step-1-start-the-protocol-engine)
  - [2. Start the Receiver](#step-2-start-the-receiver-user2)
  - [3. Start the Sender](#step-3-start-the-sender-user1)
  - [4. Verify Integrity](#step-4-verify-data-integrity)

---

## 💡 Overview

Standard UDP is fast but unreliable: datagrams can be dropped, duplicated, delayed, or delivered out of order. KTP bridges this gap by wrapping raw UDP sockets inside a custom protocol layer that guarantees:

* **Guaranteed In-Order Delivery**: Application reads byte streams sequentially without gaps.
* **Flow Control**: Dynamic receiver advertised window (`rwnd`) prevents the sender from overwhelming the receiver's buffers.
* **Loss Recovery**: Per-packet timeout tracking and automatic retransmission of unacknowledged packets.
* **Process Independence**: Socket tables live in POSIX shared memory, decoupled from user processes.
* **Connection Lifecycle**: Clean connection teardown using a two-way `FIN` / `FAK` handshake.

---

## 🏛️ Architecture & Design

```
+-------------------------------------------------------------+
|                      User Space Apps                        |
|   +--------------------+              +-----------------+   |
|   |  user1.c (Sender)  |              | user2.c (Recv)  |   |
|   +---------+----------+              +--------+--------+   |
|             | k_sendto()                       | k_recvfrom()|
+-------------|----------------------------------|------------+
              |                                  |
              v                                  v
+-------------------------------------------------------------+
|               POSIX Shared Memory (`ktp_sock[]`)            |
|  - Circular Send Buffer (`sbuf`)                            |
|  - Circular Receive Buffer (`rbuf`)                         |
|  - Send & Receive Sliding Windows (`swnd`, `rwnd`)          |
|  - Binary Semaphores for Per-Socket Mutual Exclusion        |
+-------------------------------------------------------------+
              ^                                  ^
              | Read/Write State                 | Read/Write State
+-------------|----------------------------------|------------+
|             v                                  v            |
|                 initk Background Daemon                     |
|                                                             |
|   +-------------------+  +-------------------+  +-------+   |
|   | threadR (Receiver)|  | threadS (Sender)  |  |threadG|   |
|   | - select() on UDP |  | - Retransmission  |  |  (GC) |   |
|   | - ACK generation  |  | - Timeout checks  |  |       |   |
|   | - Window updates  |  | - Sends pending   |  |       |   |
|   +---------+---------+  +---------+---------+  +-------+   |
+-------------|----------------------|------------------------+
              |                      |
              v                      v
+-------------------------------------------------------------+
|                   Underlying UDP Network                    |
+-------------------------------------------------------------+
```

### 1. Shared Memory IPC
KTP stores all socket entries (`ktp_sock`) in a shared memory segment identified via `ftok()`. Each entry contains:
* Socket metadata (bound addresses, owner PID, underlying UDP file descriptor).
* Circular buffers: `sbuf[N][512]` (send buffer) and `rbuf[N][512]` (receive buffer).
* Sliding window state: `swnd` and `rwnd` tracking head indices, sequence numbers, and deadlines.

### 2. Concurrency & Synchronization
Access to shared memory slots is guarded by a set of System V binary semaphores (`sem_lock` / `sem_unlock`). Only one thread or process may inspect or mutate a socket's state at any given moment.

### 3. The `initk` Multi-Threaded Engine
The protocol daemon (`initk`) spawns three cooperating threads:
* **`threadR` (Receiver & ACK Handler)**:
  * Uses `select()` to multiplex I/O across all active UDP sockets.
  * On `DATA`: Validates sequence numbers, places payload into `rbuf`, slides the receive window, and transmits an `ACK` with the current advertised window size.
  * On `ACK`: Slides the send window forward, frees acknowledged `sbuf` slots, and adjusts `swnd.avail`.
  * On `FIN`: Emits `FAK` and marks the socket slot for reclamation.
* **`threadS` (Sender & Retransmission Engine)**:
  * Loops periodically every $T/2$ seconds.
  * Re-transmits any unacknowledged frames whose retransmission deadline has expired.
  * First-transmits new pending messages deposited into `sbuf` by `k_sendto()`.
  * Orchestrates `FIN` retransmissions during socket close.
* **`threadG` (Garbage Collector)**:
  * Periodically verifies owner process existence via `kill(pid, 0)`.
  * Closes orphaned sockets if an application crashes without calling `k_close()`.

---

## 📦 Packet Format

Each packet sent over the wire is fixed at `PKT_LEN` (520 bytes):

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Type (4 bytes)                          |
|         "DATA"  |  "ACK\0"  |  "FIN\0"  |  "FAK\0"            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       Sequence Number         |     Advertised Window Size    |
|       (16-bit uint)           |          (16-bit uint)        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
|                    Payload (512 bytes)                        |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

---

## 🔧 API Reference

KTP provides an interface mimicking the standard POSIX Berkeley sockets API:

```c
#include "ksocket.h"

// 1. Create a KTP socket
ktp_fd k_socket(int domain, int type, int protocol);

// 2. Bind local and destination endpoints
int k_bind(ktp_fd fd, const char *src_ip, int src_port, const char *dst_ip, int dst_port);

// 3. Send message through the sliding window
ssize_t k_sendto(ktp_fd fd, const void *buf, size_t len, int flags,
                 const struct sockaddr *dst, socklen_t dstlen);

// 4. Retrieve in-order delivered message
ssize_t k_recvfrom(ktp_fd fd, void *buf, size_t len, int flags,
                   struct sockaddr *src, socklen_t *srclen);

// 5. Gracefully close the socket (triggers FIN/FAK handshake)
int k_close(ktp_fd fd);
```

---

## 🔄 Sliding Window & Flow Control

KTP maintains explicit sliding windows on both endpoints:

* **Send Window (`swnd`)**:
  * Tracks unacknowledged in-flight packets.
  * Bounded by $\min(\text{WIN\_SZ}, \text{receiver advertised window})$.
  * Advances when cumulative or selective ACKs arrive.
* **Receive Window (`rwnd`)**:
  * Accommodates out-of-order arrivals within the window boundary.
  * Delivers packets to the user application strictly in order.
  * Advertises `rwnd.avail` in every ACK packet.
  * Handles zero-window conditions via probe ACKs once buffer space reopens.

---

## 📊 Loss Simulation & Empirical Performance

The protocol includes a probabilistic packet drop simulator `sim_drop(p)` to test reliability over lossy links.

Empirical evaluation transmitting a 100 KB test payload (~200 messages of 512 bytes) with timeout $T = 5\text{s}$:

| Drop Probability ($p$) | Total Transmissions | Avg Retransmissions / Msg |
|:----------------------:|:-------------------:|:-------------------------:|
| **0.05**               | 211                 | 1.055                     |
| **0.10**               | 224                 | 1.120                     |
| **0.15**               | 239                 | 1.195                     |
| **0.20**               | 258                 | 1.290                     |
| **0.25**               | 278                 | 1.390                     |
| **0.30**               | 304                 | 1.520                     |
| **0.35**               | 336                 | 1.680                     |
| **0.40**               | 381                 | 1.905                     |
| **0.45**               | 447                 | 2.235                     |
| **0.50**               | 548                 | 2.740                     |

> **Note**: Retransmissions scale super-linearly beyond $p = 0.35$ due to cascade timeout events on dropped ACKs and retransmissions.

---

## 📁 Project Structure

```
.
├── initksocket.c      # Protocol daemon (threadR, threadS, threadG, IPC management)
├── ksocket.c          # Core socket API implementation (k_socket, k_bind, k_sendto, etc.)
├── ksocket.h          # Data structures, packet layout, and API function prototypes
├── user1.c            # Test sender application (reads input.txt and transmits over KTP)
├── user2.c            # Test receiver application (receives KTP stream and writes to disk)
├── input.txt          # Sample payload file for file transfer testing
├── documentation.txt  # Detailed protocol specifications & test data
├── makefile           # Build scripts for static library, daemon, and test binaries
└── README.md          # Project documentation
```

---

## 🛠️ Build & Compilation

### Requirements
* GCC Compiler (`gcc`)
* POSIX compliant OS (Linux / WSL)
* POSIX Threads (`-lpthread`)
* System V IPC support (`sys/ipc.h`, `sys/shm.h`, `sys/sem.h`)

### Build Targets

```bash
# Build the static library (libksocket.a)
make

# Build the protocol daemon (initk)
make init

# Build the sender (u1) and receiver (u2) test applications
make user

# Clean build artifacts
make clean

# Deep clean (removes static library as well)
make deepclean
```

---

## 🚀 Usage Guide & Demo

Open three terminal windows (or tabs) on your Linux/WSL environment:

### Step 1: Start the Protocol Engine
In **Terminal 1**, start the background daemon:
```bash
./initk
```
*This initializes the shared memory segment, sets up semaphores, and starts the receiver, sender, and GC threads.*

### Step 2: Start the Receiver (`user2`)
In **Terminal 2**, start the receiver listening on port `8002` from sender at `8001`:
```bash
./u2 127.0.0.1 8002 127.0.0.1 8001
```

### Step 3: Start the Sender (`user1`)
In **Terminal 3**, launch the sender transmitting `input.txt`:
```bash
./u1 127.0.0.1 8001 127.0.0.1 8002
```

### Step 4: Verify Data Integrity
Once the transfer completes and `user1` reaches End-of-Stream (`EOS`), verify that the received file matches the input:
```bash
diff -s input.txt received_8002.txt
# Output: Files input.txt and received_8002.txt are identical
```

---

## 📜 License

This project is open source and available under the [MIT License](LICENSE).
