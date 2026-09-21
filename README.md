# 🚀 KTP: Reliable Transfer Protocol over UDP

[![Language: C](https://img.shields.io/badge/Language-C-00599C?logo=c&logoColor=white)](https://en.wikipedia.org/wiki/C_(programming_language))
[![Platform: POSIX / Linux](https://img.shields.io/badge/Platform-POSIX%20%2F%20Linux-FCC624?logo=linux&logoColor=black)](https://www.kernel.org/)
[![Networking: UDP](https://img.shields.io/badge/Networking-UDP%20%2F%20Sockets-2496ED)](https://en.wikipedia.org/wiki/User_Datagram_Protocol)
[![Protocol: Sliding Window](https://img.shields.io/badge/Protocol-Sliding%20Window%20%2F%20ARQ-brightgreen)](https://en.wikipedia.org/wiki/Sliding_window_protocol)

**KTP (Kernel-level Transport Protocol)** is a reliable, connection-oriented transport protocol built directly over standard UDP datagram sockets. It implements sliding window flow control, cumulative and selective acknowledgment, timeout-driven retransmission, and a graceful two-way connection termination handshake.

---

## 📑 Table of Contents

- [How KTP Works](#-how-ktp-works)
- [Protocol Architecture](#-protocol-architecture)
  - [Shared Memory IPC](#1-shared-memory-ipc)
  - [Concurrency & Synchronization](#2-concurrency--synchronization)
  - [The Background Daemon (`initk`)](#3-the-background-daemon-initk)
- [Step-by-Step: What Happens During Execution](#-step-by-step-what-happens-during-execution)
  - [1. Socket Creation (`k_socket`)](#1-socket-creation-k_socket)
  - [2. Binding Endpoints (`k_bind`)](#2-binding-endpoints-k_bind)
  - [3. Sending Data (`k_sendto`)](#3-sending-data-k_sendto)
  - [4. Packet Transmission & Retransmission (`threadS`)](#4-packet-transmission--retransmission-threads)
  - [5. Packet Arrival & ACK Handling (`threadR`)](#5-packet-arrival--ack-handling-threadr)
  - [6. Receiving Data (`k_recvfrom`)](#6-receiving-data-k_recvfrom)
  - [7. Connection Teardown (`k_close`)](#7-connection-teardown-k_close)
- [Sliding Window: Mathematical Foundation](#-sliding-window-mathematical-foundation)
  - [Sequence Number Space](#sequence-number-space)
  - [Window Size Constraint](#window-size-constraint)
  - [Window Advancement Rules](#window-advancement-rules)
- [Packet Format Specification](#-packet-format-specification)

---

## 💡 How KTP Works

Standard UDP sockets provide an unreliable, connectionless datagram service where packets can be lost, duplicated, delayed, or reordered. KTP transforms raw UDP into a reliable byte-stream channel by maintaining state machines, sliding windows, and retransmission timers in shared memory:

1. **State Independence**: The protocol state machine runs in a dedicated background daemon (`initk`) using POSIX shared memory. User processes interact with sockets through lightweight API wrappers (`k_socket`, `k_bind`, `k_sendto`, `k_recvfrom`, `k_close`).
2. **Buffering**: Circular buffers decouple application write/read speeds from wire transmission speeds.
3. **Flow Control**: Every acknowledgment carries an advertised window (`rwnd.avail`) indicating remaining buffer capacity at the receiver, preventing buffer overflow.
4. **Reliability (ARQ)**: Unacknowledged packets are monitored with wall-clock expiration timers. If an ACK does not arrive within timeout period $T$, the sender retransmits the expired frames.
5. **Ordered Delivery**: Out-of-order packets arriving within the receive window are buffered, but only delivered to the application in strict contiguous sequence order.

---

## 🏛️ Protocol Architecture

```
+-------------------------------------------------------------+
|                      User Applications                      |
|   +--------------------+              +-----------------+   |
|   |  user1.c (Sender)  |              | user2.c (Recv)  |   |
|   +---------+----------+              +--------+--------+   |
|             | k_sendto()                       | k_recvfrom()|
+-------------|----------------------------------|------------+
              |                                  |
              v                                  v
+-------------------------------------------------------------+
|               POSIX Shared Memory (`ktp_sock[]`)            |
|  - Circular Send Buffer (`sbuf[10][512]`)                   |
|  - Circular Receive Buffer (`rbuf[10][512]`)                |
|  - Sliding Window Structures (`swnd`, `rwnd`)               |
|  - Per-Socket Binary Semaphores (Mutual Exclusion)          |
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
|   | - ACK generation  |  | - Timer checks    |  |       |   |
|   | - Window updates  |  | - Wire dispatch   |  |       |   |
|   +---------+---------+  +---------+---------+  +-------+   |
+-------------|----------------------|------------------------+
              |                      |
              v                      v
+-------------------------------------------------------------+
|                   Underlying UDP Network                    |
+-------------------------------------------------------------+
```

### 1. Shared Memory IPC
A shared memory table holds up to `MAX_KTP_SOCKS` (10) socket descriptors (`ktp_sock`). Each descriptor encapsulates:
* **Socket Metadata**: Local/peer addresses, owner PID, underlying UDP file descriptor.
* **Send Buffer (`sbuf`)**: 10 slots of 512-byte message payloads.
* **Receive Buffer (`rbuf`)**: 10 slots of 512-byte message payloads.
* **Window Structures (`slide_win`)**: Head indices, sequence numbers, arrival flags, and retransmission deadlines.

### 2. Concurrency & Synchronization
System V binary semaphores enforce mutual exclusion per socket entry (`sem_lock` / `sem_unlock`). Whenever an application or daemon thread reads or mutates socket state, it holds the corresponding semaphore to eliminate race conditions.

### 3. The Background Daemon (`initk`)
The `initk` process runs three continuous threads:
* **`threadR` (Receiver & ACK Handler)**: Uses `select()` with a 100ms timeout over all bound UDP descriptors to process inbound `DATA`, `ACK`, `FIN`, and `FAK` packets.
* **`threadS` (Sender & Retransmission Engine)**: Runs periodic passes every $T/2$ seconds to transmit freshly queued messages and retransmit expired frames.
* **`threadG` (Garbage Collector)**: Wakes every $T$ seconds and probes owner processes with `kill(pid, 0)`. If a process terminated without closing its socket, `threadG` initiates cleanup.

---

## 🔄 Step-by-Step: What Happens During Execution

```
Sender (user1)            Daemon (initk)              Receiver (user2)
      |                         |                           |
      |-- k_socket() ---------->| (Allocates SM slot)       |
      |-- k_bind() ------------>| (Binds UDP socket)        |
      |                         |                           |
      |-- k_sendto(payload) --->| (Copies to sbuf, exp=-1)  |
      |                         |                           |
      |                   [threadS wakes]                   |
      |                         |-- UDP DATA (seq=1) ------>| (threadR receives)
      |                         |                           | (Stores in rbuf)
      |                         |<-- UDP ACK (seq=1, win)---| (Slides rwnd)
      |                   [threadR receives ACK]            |
      |                   (Frees sbuf[1], slides swnd)      |
      |                         |                           |
      |                         |                           |-- k_recvfrom() -> read
      |                         |                           | (Frees rbuf, opens win)
      |                         |                           |
      |-- k_close() ----------->| (Sets closed flag)        |
      |                         |-- UDP FIN --------------->|
      |                         |<-- UDP FAK ---------------|
      |                         | (Releases SM slot)        |
```

### 1. Socket Creation (`k_socket`)
* Attaches to the shared memory segment.
* Scans the socket table for an unallocated entry (`free == true`).
* Initializes `swnd` and `rwnd` with window size `WIN_SZ = 10` and initial sequence numbers $1 \dots 10$.
* Marks the slot as occupied and records the caller's PID. Returns the slot index as the socket descriptor `ktp_fd`.

### 2. Binding Endpoints (`k_bind`)
* Writes the local IP/port and remote peer IP/port into the shared memory slot.
* On the next cycle, `threadR` detects the pending binding, creates a real POSIX UDP socket (`AF_INET, SOCK_DGRAM`), binds it to the local port, and marks `bound = true`.

### 3. Sending Data (`k_sendto`)
* Validates that the destination address matches the bound peer.
* Checks if `swnd.avail > 0` and an empty send buffer slot exists.
* Copies up to 512 bytes into `sbuf[slot]`, marks `sbuf_empty[slot] = false`, and sets `expires[slot] = -1` (indicating pending initial transmission).
* Decrements `swnd.avail`. Returns the number of bytes queued.

### 4. Packet Transmission & Retransmission (`threadS`)
* **Pass 1 (Expired Frames)**: Iterates over all active send buffer slots. If a slot has been sent (`expires > 0`) and the current wall-clock time exceeds `expires`, the packet has timed out. `threadS` retransmits the `DATA` packet over UDP and resets its deadline:
  $$\text{expires} = \text{now} + T \quad (T = 5\text{s})$$
* **Pass 2 (New Frames)**: Identifies slots with `expires == -1`. Constructs a `DATA` packet with the slot's assigned sequence number, sends it via `sendto()`, and sets its initial expiration timestamp to $\text{now} + T$.

### 5. Packet Arrival & ACK Handling (`threadR`)
When a UDP datagram arrives:
* **`DATA` Packet**:
  * If the sequence number falls within the current receive window and has not been seen before, the payload is written into `rbuf` and marked `got_pkt = true`.
  * The receive window slides over all contiguous received messages.
  * An `ACK` packet is returned containing:
    * `seq`: The highest in-order sequence number acknowledged.
    * `winsz`: The current available space in `rbuf` (`rwnd.avail`).
  * If the packet is a duplicate (already received or outside the window), an immediate duplicate `ACK` is returned with the current `ack_seq` to accelerate sender synchronization.
* **`ACK` Packet**:
  * The sender locates the acknowledged sequence number in `swnd`.
  * All buffer slots up to and including the acknowledged sequence number are freed (`sbuf_empty = true`, `expires = -1`).
  * `swnd.head` advances past the acknowledged slots, and newly available sequence numbers are assigned.
  * The sender updates its available window to the receiver's advertised window: `swnd.avail = winsz`.

### 6. Receiving Data (`k_recvfrom`)
* Checks if the slot at `rwnd.head` holds an in-order delivered message (`got_pkt == true`).
* Copies the payload from `rbuf` to the user's buffer.
* Clears `got_pkt[head]`, increments `rwnd.avail`, and advances `rwnd.head`.
* If the receive buffer was previously full (`recv_full == true`), the opening of buffer space triggers a probe ACK from `threadR` to notify the sender that the window has reopened.

### 7. Connection Teardown (`k_close`)
* Sets `closed = true` in the socket slot.
* `threadS` sends a `FIN` control packet to the peer and sets a retry timer.
* Upon receiving `FIN`, the peer's `threadR` replies with `FAK` (FIN-ACK) and immediately frees its socket slot.
* Upon receiving `FAK`, the initiator releases its shared memory slot and semaphore.

---

## 📐 Sliding Window: Mathematical Foundation

The protocol uses a circular sequence number space to support continuous transmission without ambiguity.

### Sequence Number Space
Sequence numbers are represented using $k = 5$ bits:

$$S = 2^k = 32$$

The sequence numbers cycle through the range:

$$1 \le \text{seq} \le 32$$

When a slot advances, its next sequence number is computed as:

$$\text{seq}_{\text{next}} = (\text{tailSeq} \bmod S) + 1$$

where `tailSeq` (in code: `tail_seq`) tracks the last assigned sequence number.

### Window Size Constraint
In any sliding window protocol with selective buffering and cumulative acknowledgments, the sender window size $W_{\text{send}}$ and receiver window size $W_{\text{recv}}$ must satisfy:

$$W_{\text{send}} + W_{\text{recv}} \le S$$

For symmetric window configurations where $W_{\text{send}} = W_{\text{recv}} = W$:

$$2W \le S \implies W \le \frac{S}{2}$$

Substituting $S = 32$:

$$W \le \frac{32}{2} = 16$$

In KTP, the window size `WIN_SZ` is configured as:

$$W = 10$$

Since $W = 10 \le 16$, the window configuration strictly satisfies the mathematical bound. This guarantees that:
1. The receiver window never overlaps with sequence numbers of unacknowledged packets from the previous cycle.
2. Delayed or duplicate packets from a previous window iteration cannot be misidentified as new data.

### Window Advancement Rules

* **Sender Window**:
  $$[V_A, \; V_A + W_{\text{eff}} - 1] \pmod S$$
  where $V_A$ is the oldest unacknowledged sequence number, and $W_{\text{eff}} = \min(W, \text{advertised window})$.

* **Receiver Window**:
  $$[V_R, \; V_R + W - 1] \pmod S$$
  where $V_R$ is the next expected in-order sequence number.

---

## 📦 Packet Format Specification

All datagrams transmitted over the network have a fixed length of 520 bytes:

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

| Field | Length | Description |
|:------|:-------|:------------|
| **Type** | 4 bytes | Identifies packet role: `DATA`, `ACK\0`, `FIN\0`, or `FAK\0`. |
| **Sequence Number** | 2 bytes | 16-bit unsigned integer (big-endian). Identifies packet sequence or ACK reference. |
| **Advertised Window Size** | 2 bytes | 16-bit unsigned integer (big-endian). Conveys available slots in receiver buffer. |
| **Payload** | 512 bytes | Application data. Zeroed out for control packets (`ACK`, `FIN`, `FAK`). |


