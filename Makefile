# Build the sysfs GPIO simulator.
#
#   make test     — build & run the native core unit tests (works on macOS/Linux)
#   make daemon   — build the FUSE daemon (needs Linux + libfuse3-dev)
#   make all      — daemon + tests
#
# The core model is portable C11, so `make test` runs anywhere. The daemon links
# libfuse3 and is Linux-only; the Makefile detects fuse3 via pkg-config and prints
# a clear message rather than failing cryptically when it's absent.

CC      ?= cc
# gnu11 (not c11) so POSIX symbols (S_IFDIR, getuid, sockaddr_un, ...) stay visible
# under glibc without sprinkling feature-test macros everywhere.
CFLAGS  ?= -O2 -g -std=gnu11 -Wall -Wextra -Wshadow \
           -Iinclude -Isrc/sysfs -Isrc/control -Isrc/web
LDLIBS_PTHREAD := -lpthread

BUILD := build
CORE_SRC := src/core/gpio_model.c src/core/log.c

HAVE_FUSE := $(shell pkg-config --exists fuse3 2>/dev/null && echo yes)

.PHONY: all test daemon tools clean

all: test daemon

$(BUILD):
	mkdir -p $(BUILD)

# ---- native core unit tests (portable) ----------------------------------

test: $(BUILD)/test_gpio_model
	$(BUILD)/test_gpio_model

$(BUILD)/test_gpio_model: tests/test_gpio_model.c $(CORE_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS_PTHREAD)

# ---- FUSE daemon + integration test tools (Linux) -----------------------

ifeq ($(HAVE_FUSE),yes)
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3)
FUSE_LIBS   := $(shell pkg-config --libs fuse3)
DAEMON_SRC  := $(CORE_SRC) src/sysfs/gpio_sysfs.c src/control/control_sock.c \
               src/web/web_server.c src/main.c

daemon: $(BUILD)/gpio-sim

$(BUILD)/gpio-sim: $(DAEMON_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $^ $(FUSE_LIBS) $(LDLIBS_PTHREAD)

tools: $(BUILD)/poll_client $(BUILD)/sim_ctl $(BUILD)/ws_probe

$(BUILD)/poll_client: tests/integration/poll_client.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/sim_ctl: tests/integration/sim_ctl.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/ws_probe: tests/integration/ws_probe.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<
else
daemon:
	@echo "==> fuse3 not found. The daemon needs Linux + libfuse3-dev (pkg-config fuse3)."
	@echo "    On macOS, build/test it in Docker:  ./tests/integration/run_docker_it.sh"
	@exit 1
tools: daemon
endif

clean:
	rm -rf $(BUILD)
