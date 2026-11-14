/*
 * xv6fuse -- mount an xv6 fs.img on Linux with libfuse 3.
 *
 *   ./xv6fuse fs.img /mnt/point [-f] [-d] ...
 *
 * The image is mmap()ed MAP_SHARED, so every change goes straight to the
 * file. The xv6 log is bypassed (we are the only writer), so unmount cleanly
 * (fusermount3 -u) before booting xv6 on the image. Runs single-threaded (-s
 * is injected) so no locking is needed.
 *
 * Supported: getattr readdir read write create mkdir rmdir unlink truncate
 *            statfs fsync (+ no-op chmod/chown/utimens so cp/touch work).
 * Not supported: rename, link, symlink (xv6 has none / not required).
 */
#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "xv6fs.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static uint8_t *img;
static size_t imgsize;
static struct superblock *sb;
static uint32_t datastart;
static time_t mount_time;

/* ---------- raw access ---------- */

static inline uint8_t *blk(uint32_t n) { return img + (size_t)n * BSIZE; }

static struct dinode *iget(uint32_t inum) {
  if (inum == 0 || inum >= sb->ninodes) return NULL;
  return (struct dinode *)blk(IBLOCK(inum, *sb)) + inum % IPB;
}

/* ---------- block allocator (bitmap) ---------- */

static uint32_t balloc(void) {
  for (uint32_t b = datastart; b < sb->size; b++) {
    uint8_t *bm = blk(BBLOCK(b, *sb));
    uint32_t bi = b % BPB;
    uint8_t m = 1 << (bi % 8);
    if (!(bm[bi / 8] & m)) {
      bm[bi / 8] |= m;
      memset(blk(b), 0, BSIZE);
      return b;
    }
  }
  return 0;
}

static void bfree(uint32_t b) {
  if (b < datastart || b >= sb->size) return;
  uint8_t *bm = blk(BBLOCK(b, *sb));
  uint32_t bi = b % BPB;
  bm[bi / 8] &= ~(1 << (bi % 8));
}

/* ---------- inode block map (direct + one indirect) ---------- */

/* Return disk block for file block bn; allocate if alloc. 0 = hole / no space. */
static uint32_t bmap(struct dinode *ip, uint32_t bn, int alloc) {
  if (bn < NDIRECT) {
    if (!ip->addrs[bn] && alloc) ip->addrs[bn] = balloc();
    return ip->addrs[bn];
  }
  bn -= NDIRECT;
  if (bn >= NINDIRECT) return 0;
  if (!ip->addrs[NDIRECT]) {
    if (!alloc) return 0;
    if (!(ip->addrs[NDIRECT] = balloc())) return 0;
  }
  uint32_t *a = (uint32_t *)blk(ip->addrs[NDIRECT]);
  if (!a[bn] && alloc) a[bn] = balloc();
  return a[bn];
}

static int readi(struct dinode *ip, char *dst, uint32_t off, uint32_t n) {
  if (off >= ip->size) return 0;
  if (n > ip->size - off) n = ip->size - off;
  uint32_t tot = 0;
  while (tot < n) {
    uint32_t pos = off + tot;
    uint32_t m = MIN(n - tot, BSIZE - pos % BSIZE);
    uint32_t b = bmap(ip, pos / BSIZE, 0);
    if (b) memcpy(dst + tot, blk(b) + pos % BSIZE, m);
    else   memset(dst + tot, 0, m);            /* hole */
    tot += m;
  }
  return tot;
}

/* Returns bytes written, or -errno if nothing could be written. */
static int writei(struct dinode *ip, const char *src, uint32_t off, uint32_t n) {
  const uint64_t max = (uint64_t)MAXFILE * BSIZE;
  if (off >= max) return n ? -EFBIG : 0;
  if (n > max - off) n = max - off;
  uint32_t tot = 0;
  while (tot < n) {
    uint32_t pos = off + tot;
    uint32_t m = MIN(n - tot, BSIZE - pos % BSIZE);
    uint32_t b = bmap(ip, pos / BSIZE, 1);
    if (!b) break;                              /* disk full */
    memcpy(blk(b) + pos % BSIZE, src + tot, m);
    tot += m;
  }
  if (tot == 0 && n > 0) return -ENOSPC;
  if (tot > 0 && off + tot > ip->size) ip->size = off + tot;
  return tot;
}

/* Shrink/grow to ns. Invariant: bytes past EOF inside the last block are 0. */
static int itrunc(struct dinode *ip, uint64_t ns) {
  if (ns > (uint64_t)MAXFILE * BSIZE) return -EFBIG;
  if (ns < ip->size) {
    uint32_t keep = (ns + BSIZE - 1) / BSIZE;   /* blocks that must remain */
    for (uint32_t i = keep; i < NDIRECT; i++)
      if (ip->addrs[i]) { bfree(ip->addrs[i]); ip->addrs[i] = 0; }
    if (ip->addrs[NDIRECT]) {
      uint32_t *a = (uint32_t *)blk(ip->addrs[NDIRECT]);
      for (uint32_t j = 0; j < NINDIRECT; j++)
        if (NDIRECT + j >= keep && a[j]) { bfree(a[j]); a[j] = 0; }
      if (keep <= NDIRECT) { bfree(ip->addrs[NDIRECT]); ip->addrs[NDIRECT] = 0; }
    }
    if (ns % BSIZE) {
      uint32_t b = bmap(ip, ns / BSIZE, 0);
      if (b) memset(blk(b) + ns % BSIZE, 0, BSIZE - ns % BSIZE);
    }
  }
  ip->size = (uint32_t)ns;                      /* growing = sparse hole */
  return 0;
}

/* ---------- inodes ---------- */

static uint32_t ialloc(int type) {
  for (uint32_t i = 1; i < sb->ninodes; i++) {
    struct dinode *ip = iget(i);
    if (ip->type == 0) {
      memset(ip, 0, sizeof *ip);
      ip->type = type;
      ip->nlink = 1;
      return i;
    }
  }
  return 0;
}

static void ifree(struct dinode *ip) {
  itrunc(ip, 0);
  memset(ip, 0, sizeof *ip);
}

/* ---------- directories & paths ---------- */

static uint32_t dirlookup(struct dinode *dp, const char *name, uint32_t *poff) {
  struct dirent de;
  if (strlen(name) > DIRSIZ) return 0;
  for (uint32_t off = 0; off + sizeof de <= dp->size; off += sizeof de) {
    readi(dp, (char *)&de, off, sizeof de);
    if (de.inum && !strncmp(de.name, name, DIRSIZ)) {
      if (poff) *poff = off;
      return de.inum;
    }
  }
  return 0;
}

static int dirlink(struct dinode *dp, const char *name, uint32_t inum) {
  struct dirent de;
  uint32_t off;
  for (off = 0; off + sizeof de <= dp->size; off += sizeof de) {
    readi(dp, (char *)&de, off, sizeof de);
    if (de.inum == 0) break;                    /* reuse free slot */
  }
  memset(&de, 0, sizeof de);
  de.inum = inum;
  memcpy(de.name, name, strlen(name));          /* strlen <= DIRSIZ; rest stays 0 */
  return writei(dp, (char *)&de, off, sizeof de) == sizeof de ? 0 : -ENOSPC;
}

static uint32_t namei(const char *path) {
  uint32_t inum = ROOTINO;
  const char *p = path;
  char name[DIRSIZ + 1];
  while (*p) {
    while (*p == '/') p++;
    if (!*p) break;
    const char *s = p;
    while (*p && *p != '/') p++;
    size_t l = p - s;
    if (l > DIRSIZ) return 0;
    struct dinode *ip = iget(inum);
    if (!ip || ip->type != T_DIR) return 0;
    memcpy(name, s, l);
    name[l] = 0;
    if (!(inum = dirlookup(ip, name, NULL))) return 0;
  }
  return inum;
}

/* Split path into parent dir inode + last component. */
static int namep(const char *path, uint32_t *dir, char *name) {
  const char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) return -EINVAL;
  if (strlen(slash + 1) > DIRSIZ) return -ENAMETOOLONG;
  strcpy(name, slash + 1);
  char tmp[PATH_MAX];
  size_t dl = slash - path;
  if (dl >= sizeof tmp) return -ENAMETOOLONG;
  memcpy(tmp, path, dl);
  tmp[dl] = 0;
  uint32_t d = namei(tmp);
  struct dinode *dp = iget(d);
  if (!dp) return -ENOENT;
  if (dp->type != T_DIR) return -ENOTDIR;
  *dir = d;
  return 0;
}

/* ---------- FUSE operations ---------- */

static int x_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
  (void)fi;
  uint32_t inum = namei(path);
  struct dinode *ip = iget(inum);
  if (!ip) return -ENOENT;
  memset(st, 0, sizeof *st);
  st->st_ino = inum;
  switch (ip->type) {
  case T_DIR: st->st_mode = S_IFDIR | 0755; st->st_nlink = ip->nlink + 1; break;
  case T_DEV: st->st_mode = S_IFCHR | 0666; st->st_nlink = ip->nlink;
              st->st_rdev = makedev(ip->major, ip->minor); break;
  default:    st->st_mode = S_IFREG | 0644; st->st_nlink = ip->nlink; break;
  }
  st->st_size = ip->size;
  st->st_blksize = BSIZE;
  st->st_blocks = (ip->size + 511) / 512;
  st->st_uid = fuse_get_context()->uid;
  st->st_gid = fuse_get_context()->gid;
  st->st_atime = st->st_mtime = st->st_ctime = mount_time;
  return 0;
}

static int x_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off,
                     struct fuse_file_info *fi, enum fuse_readdir_flags fl) {
  (void)off; (void)fi; (void)fl;
  struct dinode *ip = iget(namei(path));
  if (!ip) return -ENOENT;
  if (ip->type != T_DIR) return -ENOTDIR;
  struct dirent de;
  char nm[DIRSIZ + 1];
  for (uint32_t o = 0; o + sizeof de <= ip->size; o += sizeof de) {
    readi(ip, (char *)&de, o, sizeof de);
    if (!de.inum) continue;
    memcpy(nm, de.name, DIRSIZ);
    nm[DIRSIZ] = 0;
    if (fill(buf, nm, NULL, 0, 0)) break;
  }
  return 0;
}

static int x_read(const char *path, char *buf, size_t size, off_t off,
                  struct fuse_file_info *fi) {
  (void)fi;
  struct dinode *ip = iget(namei(path));
  if (!ip) return -ENOENT;
  if (ip->type == T_DIR) return -EISDIR;
  if (ip->type == T_DEV) return -ENODEV;
  if (off < 0 || (uint64_t)off >= ip->size) return 0;
  return readi(ip, buf, off, size);
}

static int x_write(const char *path, const char *buf, size_t size, off_t off,
                   struct fuse_file_info *fi) {
  (void)fi;
  struct dinode *ip = iget(namei(path));
  if (!ip) return -ENOENT;
  if (ip->type == T_DIR) return -EISDIR;
  if (ip->type == T_DEV) return -ENODEV;
  if (off < 0 || (uint64_t)off > (uint64_t)MAXFILE * BSIZE) return -EFBIG;
  return writei(ip, buf, off, size);
}

/* libfuse enables FUSE_CAP_ATOMIC_O_TRUNC, so open(O_TRUNC) arrives here
 * (not as a separate truncate call). */
static int x_open(const char *path, struct fuse_file_info *fi) {
  struct dinode *ip = iget(namei(path));
  if (!ip) return -ENOENT;
  if (ip->type == T_DIR) return -EISDIR;
  if (fi->flags & O_TRUNC) return itrunc(ip, 0);
  return 0;
}

static int x_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
  (void)mode; (void)fi;
  uint32_t dpn;
  char name[DIRSIZ + 1];
  int r = namep(path, &dpn, name);
  if (r) return r;
  struct dinode *dp = iget(dpn);
  if (dirlookup(dp, name, NULL)) return -EEXIST;
  uint32_t n = ialloc(T_FILE);
  if (!n) return -ENOSPC;
  if (dirlink(dp, name, n)) { ifree(iget(n)); return -ENOSPC; }
  return 0;
}

static int x_mkdir(const char *path, mode_t mode) {
  (void)mode;
  uint32_t dpn;
  char name[DIRSIZ + 1];
  int r = namep(path, &dpn, name);
  if (r) return r;
  struct dinode *dp = iget(dpn);
  if (dirlookup(dp, name, NULL)) return -EEXIST;
  uint32_t n = ialloc(T_DIR);
  if (!n) return -ENOSPC;
  struct dinode *ip = iget(n);
  /* xv6 convention: "." is not counted, ".." counts toward the parent. */
  if (dirlink(ip, ".", n) || dirlink(ip, "..", dpn) || dirlink(dp, name, n)) {
    ifree(ip);
    return -ENOSPC;
  }
  dp->nlink++;
  return 0;
}

static int clear_entry(struct dinode *dp, uint32_t off) {
  struct dirent z;
  memset(&z, 0, sizeof z);
  return writei(dp, (char *)&z, off, sizeof z) == sizeof z ? 0 : -EIO;
}

static int x_unlink(const char *path) {
  uint32_t dpn, off;
  char name[DIRSIZ + 1];
  int r = namep(path, &dpn, name);
  if (r) return r;
  struct dinode *dp = iget(dpn);
  uint32_t n = dirlookup(dp, name, &off);
  if (!n) return -ENOENT;
  struct dinode *ip = iget(n);
  if (ip->type == T_DIR) return -EISDIR;
  if ((r = clear_entry(dp, off))) return r;
  if (--ip->nlink <= 0) ifree(ip);
  return 0;
}

static int x_rmdir(const char *path) {
  uint32_t dpn, off;
  char name[DIRSIZ + 1];
  int r = namep(path, &dpn, name);
  if (r) return r;
  struct dinode *dp = iget(dpn);
  uint32_t n = dirlookup(dp, name, &off);
  if (!n) return -ENOENT;
  struct dinode *ip = iget(n);
  if (ip->type != T_DIR) return -ENOTDIR;
  struct dirent de;
  for (uint32_t o = 0; o + sizeof de <= ip->size; o += sizeof de) {
    readi(ip, (char *)&de, o, sizeof de);
    if (de.inum && strncmp(de.name, ".", DIRSIZ) && strncmp(de.name, "..", DIRSIZ))
      return -ENOTEMPTY;
  }
  if ((r = clear_entry(dp, off))) return r;
  dp->nlink--;                                  /* child's ".." is gone */
  ifree(ip);
  return 0;
}

static int x_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
  (void)fi;
  struct dinode *ip = iget(namei(path));
  if (!ip) return -ENOENT;
  if (ip->type == T_DIR) return -EISDIR;
  if (size < 0) return -EINVAL;
  return itrunc(ip, size);
}

static int x_chmod(const char *p, mode_t m, struct fuse_file_info *fi) { (void)p; (void)m; (void)fi; return 0; }
static int x_chown(const char *p, uid_t u, gid_t g, struct fuse_file_info *fi) { (void)p; (void)u; (void)g; (void)fi; return 0; }
static int x_utimens(const char *p, const struct timespec tv[2], struct fuse_file_info *fi) { (void)p; (void)tv; (void)fi; return 0; }

static int x_fsync(const char *p, int ds, struct fuse_file_info *fi) {
  (void)p; (void)ds; (void)fi;
  return msync(img, imgsize, MS_SYNC) ? -errno : 0;
}

static int x_statfs(const char *path, struct statvfs *s) {
  (void)path;
  uint32_t freeb = 0, freei = 0;
  for (uint32_t b = datastart; b < sb->size; b++)
    if (!(blk(BBLOCK(b, *sb))[(b % BPB) / 8] & (1 << (b % 8)))) freeb++;
  for (uint32_t i = 1; i < sb->ninodes; i++)
    if (iget(i)->type == 0) freei++;
  memset(s, 0, sizeof *s);
  s->f_bsize = s->f_frsize = BSIZE;
  s->f_blocks = sb->nblocks;
  s->f_bfree = s->f_bavail = freeb;
  s->f_files = sb->ninodes - 1;
  s->f_ffree = s->f_favail = freei;
  s->f_namemax = DIRSIZ;
  return 0;
}

static void x_destroy(void *priv) {
  (void)priv;
  msync(img, imgsize, MS_SYNC);
  munmap(img, imgsize);
}

static const struct fuse_operations ops = {
  .getattr  = x_getattr,
  .readdir  = x_readdir,
  .open     = x_open,
  .read     = x_read,
  .write    = x_write,
  .create   = x_create,
  .mkdir    = x_mkdir,
  .rmdir    = x_rmdir,
  .unlink   = x_unlink,
  .truncate = x_truncate,
  .chmod    = x_chmod,
  .chown    = x_chown,
  .utimens  = x_utimens,
  .fsync    = x_fsync,
  .statfs   = x_statfs,
  .destroy  = x_destroy,
};

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s fs.img mountpoint [fuse options]\n", argv[0]);
    return 2;
  }
  int fd = open(argv[1], O_RDWR);
  if (fd < 0) { perror(argv[1]); return 1; }
  struct stat st;
  if (fstat(fd, &st) < 0) { perror("fstat"); return 1; }
  imgsize = st.st_size;
  if (imgsize < 2 * BSIZE) { fprintf(stderr, "image too small\n"); return 1; }
  img = mmap(NULL, imgsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (img == MAP_FAILED) { perror("mmap"); return 1; }
  sb = (struct superblock *)blk(1);

  uint32_t nib = (sb->ninodes + IPB - 1) / IPB, nbm = (sb->size + BPB - 1) / BPB;
  if (sb->size == 0 || (uint64_t)sb->size * BSIZE > imgsize || sb->nblocks >= sb->size ||
      sb->ninodes < 2 || sb->inodestart + nib > sb->bmapstart ||
      sb->bmapstart + nbm > sb->size - sb->nblocks) {
    fprintf(stderr, "%s: doesn't look like an xv6 image (BSIZE=%d)\n", argv[1], BSIZE);
    return 1;
  }
  datastart = sb->size - sb->nblocks;
  mount_time = time(NULL);

  /* fuse args: argv[0], -s (single thread), then everything after the image */
  char **nargv = malloc((argc + 1) * sizeof *nargv);
  int nargc = 0;
  nargv[nargc++] = argv[0];
  nargv[nargc++] = "-s";
  for (int i = 2; i < argc; i++) nargv[nargc++] = argv[i];
  nargv[nargc] = NULL;
  return fuse_main(nargc, nargv, &ops, NULL);
}
