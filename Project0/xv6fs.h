/* On-disk layout of xv6 (x86, xv6-public). Copied from fs.h.
 * If your xv6 uses a different BSIZE (e.g. 1024 in the RISC-V version) or a
 * different superblock, change it here -- every tool below includes this. */
#ifndef XV6FS_H
#define XV6FS_H
#include <stdint.h>

#define BSIZE     512
#define ROOTINO   1
#define NDIRECT   12
#define NINDIRECT (BSIZE / 4)
#define MAXFILE   (NDIRECT + NINDIRECT)

#define T_DIR  1
#define T_FILE 2
#define T_DEV  3

struct superblock {
  uint32_t size;       /* total blocks in image */
  uint32_t nblocks;    /* number of data blocks */
  uint32_t ninodes;
  uint32_t nlog;
  uint32_t logstart;
  uint32_t inodestart;
  uint32_t bmapstart;
};

struct dinode {
  int16_t  type;
  int16_t  major;
  int16_t  minor;
  int16_t  nlink;
  uint32_t size;
  uint32_t addrs[NDIRECT + 1];
};

#define IPB           (BSIZE / sizeof(struct dinode))
#define IBLOCK(i, sb) ((i) / IPB + (sb).inodestart)
#define BPB           (BSIZE * 8)
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

#define DIRSIZ 14
struct dirent {
  uint16_t inum;
  char     name[DIRSIZ];
};

#endif
