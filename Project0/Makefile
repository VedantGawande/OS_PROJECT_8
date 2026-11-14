CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2 -g
FUSE_CF := $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LD := $(shell pkg-config --libs fuse3 2>/dev/null)

all: xv6fuse xv6fsck mkimg

xv6fuse: xv6fuse.c xv6fs.h
	$(CC) $(CFLAGS) $(FUSE_CF) -o $@ $< $(FUSE_LD)

xv6fsck: xv6fsck.c xv6fs.h
	$(CC) $(CFLAGS) -o $@ $<

mkimg: mkimg.c xv6fs.h
	$(CC) $(CFLAGS) -o $@ $<

test: all
	./test.sh

clean:
	rm -f xv6fuse xv6fsck mkimg
