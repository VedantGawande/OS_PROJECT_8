# xv6 FUSE (Project 0)

    sudo apt install libfuse3-dev fuse3 build-essential
    make                       # xv6fuse (driver), xv6fsck, mkimg (mini mkfs for testing)
    ./test.sh                  # self-contained Linux-side test (34 checks) ; ./test.sh /path/xv6/fs.img uses a copy

    mkdir mnt && ./xv6fuse fs.img mnt     # add -f to stay in foreground, -d to debug
    cp hello.txt mnt/ ; mkdir mnt/docs ; ls mnt
    fusermount3 -u mnt                    # ALWAYS unmount before booting xv6
    ./xv6fsck fs.img

If your xv6 has a different BSIZE/superblock (e.g. the RISC-V one), edit `xv6fs.h` only.

## Linux -> xv6
    cp /path/to/xv6/fs.img . ; ./xv6fuse fs.img mnt
    cp notes.txt mnt/ ; head -c 60000 /dev/urandom | base64 > /tmp/b ; cp /tmp/b mnt/big.txt   # use <= 71680 bytes (MAXFILE)
    fusermount3 -u mnt ; ./xv6fsck fs.img
    qemu-system-i386 -nographic -drive file=fs.img,index=1,media=disk,format=raw \
        -drive file=xv6.img,index=0,media=disk,format=raw -smp 2 -m 512
    $ cat notes.txt          # (don't run `make qemu` -- it may rebuild fs.img)

## xv6 -> Linux
In xv6: `echo hello from xv6 > a.txt`, `mkdir d`, `cat README > d/r`, then exit qemu (Ctrl-a x), then
    ./xv6fsck fs.img ; ./xv6fuse fs.img mnt ; cat mnt/a.txt ; diff mnt/d/r /path/to/xv6/README ; fusermount3 -u mnt

## Design notes
* Image is mmap'd MAP_SHARED; single-threaded (-s injected); xv6 log is bypassed (logs are empty after a clean xv6 shutdown).
* Bitmap: `balloc` scans from the first data block, sets the bit and zeroes the block; `bfree` clears it. Indirect block allocated lazily, freed when a truncate drops below NDIRECT blocks.
* Invariant: bytes past EOF in the last block are zero (zeroed on shrink), so grow/sparse writes read back zeros.
* nlink: "." not counted, ".." counted (xv6 convention); mkdir does parent->nlink++, rmdir does --.
* Max file 140 blocks = 71680 bytes, names <= 14 chars (ENAMETOOLONG / EFBIG / ENOSPC returned).
* No rename/link (not required).
