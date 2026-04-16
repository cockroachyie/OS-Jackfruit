# OS-Jackfruit — Unified Makefile
# ================================
# Builds: engine (user-space supervisor), monitor.ko (LKM),
#         and workload binaries (cpu_bound, io_bound).
#
# Usage:
#   make              — build everything (engine without LKM integration)
#   make MONITOR=1    — build engine WITH ioctl integration for monitor.ko
#   make clean        — remove all build artefacts
#
# Full workflow (see README.md for step-by-step):
#   make MONITOR=1
#   sudo insmod monitor.ko
#   ls -l /dev/container_monitor
#   sudo ./engine supervisor ./rootfs &
#   sudo ./engine start alpha ./rootfs /bin/sh
#   sudo ./engine ps
#   sudo rmmod monitor

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread

KVER   ?= $(shell uname -r)
KDIR   ?= /lib/modules/$(KVER)/build
PWD    := $(shell pwd)

.PHONY: all engine workloads module clean

all: engine workloads module

# ---- User-space engine ----
engine: engine.c monitor_ioctl.h
ifdef MONITOR
	$(CC) $(CFLAGS) -DHAVE_MONITOR -o engine engine.c $(LDFLAGS)
	@echo "[+] engine built WITH kernel monitor support"
else
	$(CC) $(CFLAGS) -o engine engine.c $(LDFLAGS)
	@echo "[+] engine built (no kernel monitor — run 'make MONITOR=1' to enable)"
endif

# ---- Workload binaries ----
workloads: cpu_bound io_bound

cpu_bound: workloads/cpu_bound.c
	$(CC) $(CFLAGS) -o cpu_bound workloads/cpu_bound.c -lm
	@echo "[+] cpu_bound built"

io_bound: workloads/io_bound.c
	$(CC) $(CFLAGS) -o io_bound workloads/io_bound.c
	@echo "[+] io_bound built"

# ---- Kernel module ----
module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@echo "[+] monitor.ko built"

# ---- Clean ----
clean:
	rm -f engine cpu_bound io_bound
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	@echo "[+] clean done"
