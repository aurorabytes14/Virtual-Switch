# Virtual Switch

A multithreaded software network switch implemented in C using POSIX threads and TCP sockets. The project simulates Ethernet-style frame forwarding between connected hosts and includes MAC learning, VLAN-aware switching, Quality of Service (QoS), traffic shaping, port mirroring, and an administrative interface.

## 1. Project Overview

The virtual switch accepts multiple host connections over TCP. Each connected host is assigned a switch port. Hosts exchange frames through the switch, which learns source MAC addresses and forwards frames according to its MAC address table and VLAN configuration.

The implementation focuses on concurrent packet processing, bounded queues, thread synchronization, memory management, and isolation between slow ports.

## 2. Project Structure

```text
.
├── src/
│   ├── admin_commands.c
│   ├── admin_commands.h
│   ├── admin_server.c
│   ├── admin_server.h
│   ├── frame.c
│   ├── frame.h
│   ├── frame_io.c
│   ├── frame_io.h
│   ├── host.c
│   ├── mac_table.c
│   ├── mac_table.h
│   ├── mirror_queue.c
│   ├── mirror_queue.h
│   ├── port_queue.c
│   ├── port_queue.h
│   ├── shared_frame.c
│   ├── shared_frame.h
│   ├── shm_stats.c
│   ├── shm_stats.h
│   ├── sniffer.py
│   ├── swctl.c
│   └── switch.c
├── test/
│   ├── test_admin_commands.c
│   ├── test_admin_server.c
│   ├── test_disconnect.c
│   ├── test_frame_io.c
│   ├── test_mac.c
│   ├── test_mirror_isolation.c
│   ├── test_mirror_queue.c
│   ├── test_port_queue.c
│   ├── test_qos.c
│   ├── test_queue_concurrent.c
│   ├── test_rate_limit.c
│   ├── test_shm_stats.c
│   ├── test_slow_port.c
│   └── test_vlan_mac.c
├── Makefile
├── .gitignore
└── README.md
```

## 3. Requirements

- Linux
- GCC
- GNU Make
- POSIX threads
- POSIX shared memory support

## 4. Building

Build all application executables:

```bash
make clean
make all
```

This produces:

- `switch` — the virtual switch
- `host` — the host simulator
- `swctl` — the administrative command-line client

The project uses the following compiler flags:

```text
-std=c11 -Wall -Wextra -Werror -g -O0
```

## 5. Running the Application

### Start the switch

```bash
./switch 9000 3
```

Arguments:

- `9000`: TCP listening port
- `3`: maximum number of connected switch ports

### Connect hosts

Open separate terminals and run:

```bash
./host 127.0.0.1 9000 AA:AA:AA:AA:AA:01
```

```bash
./host 127.0.0.1 9000 AA:AA:AA:AA:AA:02
```

An optional VLAN ID can be supplied:

```bash
./host 127.0.0.1 9000 AA:AA:AA:AA:AA:03 10
```

Hosts can exchange messages through the switch.

### Stop the switch

Press `Ctrl+C` in the switch terminal. The switch shuts down its listening socket, connected ports, worker threads, and associated resources.

## 6. Implemented Features

### Frame Protocol and I/O

Frames use a fixed 17-byte header followed by a variable-length payload.

| Field | Size |
|---|---:|
| Payload length | 2 bytes |
| Source MAC | 6 bytes |
| Destination MAC | 6 bytes |
| VLAN ID | 2 bytes |
| Priority | 1 byte |
| Payload | 0–1500 bytes |

The implementation handles partial reads and writes, interrupted system calls, invalid frame lengths, and disconnected peers. `MSG_NOSIGNAL` is used to avoid process termination from `SIGPIPE` during socket writes.

### MAC Address Learning

The switch learns source MAC addresses and associates them with their ingress ports and VLANs.

The MAC table supports:

- VLAN-aware lookups and insertion.
- Multiple hash buckets with striped mutexes.
- MAC address aging.
- LRU eviction when the table reaches capacity.
- MAC movement detection and logging.
- Per-port MAC address limits and port security callbacks.

### Concurrent Port Queues

Each connected port has a bounded output queue and a dedicated sender thread.

Frames are enqueued without making the receiving thread wait for a slow destination. When a queue is full, the frame is dropped and the drop counter is updated.

Shared frames use atomic reference counting so multiple output queues can safely reference the same frame.

### Port Admission Control

A counting semaphore limits the number of connected hosts. Timed admission waits are used, and connections that cannot obtain a port slot receive a `switch full` response.

### VLAN Support

The switch supports VLAN-aware MAC learning and forwarding.

- Access ports assign their configured VLAN to incoming frames.
- Trunk ports retain the VLAN carried by incoming frames.
- Forwarding is restricted to ports that accept the frame's VLAN.

Example configuration:

```bash
SWITCH_PORT_VLANS=10,20,1 SWITCH_TRUNK_PORT=2 ./switch 9000 3
```

This configures access VLAN 10 on port 0, access VLAN 20 on port 1, and a trunk on port 2.

### Quality of Service

The queue implementation supports three priority levels:

- Priority 0: High
- Priority 1: Normal
- Priority 2: Low

Scheduling includes weighted round-robin behavior and priority aging to prevent lower-priority frames from being indefinitely postponed.

### Traffic Rate Limiting

A token-bucket mechanism controls the rate at which frames are transmitted. It supports an initial burst up to the configured bucket capacity and subsequent token replenishment.

### Port Mirroring

Selected switch ports can mirror frames to a separate sniffer connection.

Mirrored frames use a dedicated bounded queue and sender thread, keeping a slow sniffer from blocking normal port forwarding.

The included `src/sniffer.py` provides a Python-based sniffer client.

### Administrative Interface

The switch exposes a UNIX domain socket administrative interface. The `swctl` executable sends administrative commands to the running switch.

Examples:

```bash
./swctl show mac
./swctl clear mac
./swctl show ports
./swctl show mirror
./swctl mirror 0 on
./swctl mirror 0 off
```

### Shared Memory Statistics

The switch exposes port statistics through POSIX shared memory. The statistics structure includes counters for:

- Frames received.
- Frames transmitted.
- Frames dropped.
- Frames mirrored.

A process-shared mutex protects shared statistics updates.

### Signal Handling and Shutdown

Control signals are blocked before worker threads are created and handled by a dedicated signal thread using `sigwait()`.

- `SIGINT` and `SIGTERM`: initiate shutdown.
- `SIGUSR1`: print the MAC address table.
- `SIGHUP`: report that configuration reload is not configured.

## 7. Concurrency and Synchronization

The project uses POSIX threads, mutexes, condition variables, semaphores, and C11 atomic operations.

Important synchronization mechanisms include:

- Per-port queue mutexes and condition variables.
- Dedicated sender threads for output queues.
- Atomic reference counts for shared frames.
- Per-bucket mutexes for MAC table operations.
- A capacity lock for MAC table capacity management.
- A semaphore for limiting active connections.
- A mutex for shared-memory statistics.
- Atomic shutdown state shared between threads.

For MAC table operations that require global coordination, the capacity lock is acquired before bucket locks, with bucket locks acquired in ascending order.

The design aims to avoid holding unrelated locks during socket transmission and to prevent slow output destinations from blocking other ports.

## 8. Testing

Run the automated test suite:

```bash
make test
```

The test suite covers:

| Test area | Coverage |
|---|---|
| MAC table | Insertion, LRU eviction, aging, port security, MAC movement |
| Frame I/O | Serialization, transmission, invalid lengths, disconnects |
| Port queues | Delivery order, overflow, reference counting |
| Concurrent producers | Multiple producers and exactly-once delivery |
| Slow ports | Isolation of blocked output queues |
| Disconnection | Partial frames and EPIPE handling |
| Shared memory | Parent/child shared counter updates |
| Admin commands | MAC display, clearing, invalid commands |
| QoS | Weighted scheduling, priority preference, aging |
| Rate limiting | Token-bucket burst and refill behavior |
| VLAN | VLAN-aware MAC learning and isolation |
| Mirroring | Queue overflow and slow-sniffer isolation |

The administrative server integration test can be built and run separately:

```bash
make test_admin_server
make run-admin-test
```

It starts an interactive test server. Use another terminal to issue commands with `swctl`.

## 9. Verified Validation Results

The final clean build completed successfully with the configured strict compiler flags.

The automated test suite completed successfully, including the MAC table, frame I/O, queues, concurrency, shared memory, administration, QoS, rate limiting, VLAN, and mirroring tests.

Additional integration testing covered:

- Bidirectional host communication.
- Unknown-unicast flooding and MAC learning.
- Maximum connection limits and port-slot reuse.
- VLAN access-port isolation.
- VLAN trunk forwarding.
- Port mirroring and disabling mirroring.
- Shutdown with connected hosts.
- Error-triggered cleanup using GDB fault injection.

## 10. Current Limitations

- Configuration reload is not implemented; `SIGHUP` reports that reload was skipped.
- The trunk implementation is software-level VLAN forwarding, not a claim of physical switch interoperability.
- The project does not implement the full two-switch pipeline or supervisor functionality.
- The sniffer uses a separate UNIX domain socket connection.

## 11. Cleaning Build Artifacts

Remove generated application and test executables:

```bash
make clean
```