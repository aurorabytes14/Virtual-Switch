CC = gcc

CFLAGS = -std=c11 -Wall -Wextra -Werror -g -O0 -Isrc
LDFLAGS = -pthread -lrt

SRC = src/frame.c \
      src/frame_io.c \
      src/shared_frame.c \
      src/port_queue.c \
      src/mirror_queue.c \
      src/mac_table.c \
      src/shm_stats.c \
      src/admin_server.c \
      src/admin_commands.c

TESTS = test_mac \
        test_frame_io \
        test_port_queue \
        test_queue_concurrent \
        test_slow_port \
        test_disconnect \
        test_shm_stats \
        test_admin_commands \
        test_qos \
        test_rate_limit \
        test_vlan_mac \
        test_mirror_queue \
        test_mirror_isolation
.PHONY: all clean test run-admin-test

all:switch host swctl

switch: $(SRC) src/switch.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

swctl: src/swctl.c
	$(CC) $(CFLAGS) $< -o $@

host: src/frame.c src/frame_io.c src/host.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_mac: src/mac_table.c test/test_mac.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_vlan_mac: src/mac_table.c test/test_vlan_mac.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_frame_io: src/frame.c src/frame_io.c test/test_frame_io.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_port_queue: src/frame.c src/frame_io.c src/shared_frame.c src/port_queue.c test/test_port_queue.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_queue_concurrent: src/frame.c src/frame_io.c src/shared_frame.c src/port_queue.c test/test_queue_concurrent.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_slow_port: src/frame.c src/frame_io.c src/shared_frame.c src/port_queue.c test/test_slow_port.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_disconnect: src/frame.c src/frame_io.c test/test_disconnect.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_shm_stats: src/shm_stats.c test/test_shm_stats.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_admin_commands: $(SRC) test/test_admin_commands.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_admin_server: $(SRC) test/test_admin_server.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_qos: src/frame.c src/frame_io.c src/shared_frame.c src/port_queue.c test/test_qos.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_rate_limit: src/frame.c src/frame_io.c src/shared_frame.c src/port_queue.c test/test_rate_limit.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_mirror_queue: src/frame.c src/frame_io.c src/shared_frame.c src/mirror_queue.c test/test_mirror_queue.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test_mirror_isolation: src/frame.c src/frame_io.c src/shared_frame.c src/mirror_queue.c src/port_queue.c test/test_mirror_isolation.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

run-admin-test: test_admin_server
	./test_admin_server

clean:
	rm -f *.o *.out swctl switch host $(TESTS) test_admin_server