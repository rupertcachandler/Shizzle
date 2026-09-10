# NDI Test Pattern Generator — Makefile
# Requires NDI SDK v6 headers + libndi.so
# Set NDI_SDK to the SDK root path if not installed system-wide

NDI_SDK ?= /opt/ndi-monitor-v2

CC = gcc
CFLAGS = -Wall -Wextra -O2 -I$(NDI_SDK)/include
LDFLAGS = -L$(NDI_SDK)/lib -lndi -lm -Wl,-rpath,$(NDI_SDK)/lib

.PHONY: all clean install

all: ndi-testgen

ndi-testgen: ndi-testgen.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f ndi-testgen

install: all
	install -d /usr/local/bin
	install -m 755 ndi-testgen /usr/local/bin/
