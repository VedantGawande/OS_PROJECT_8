/*
 * xv6fsck -- consistency checker for an xv6 fs.img (read-only).
 *
 * Checks:
 *   1. superblock geometry
 *   2. every allocated inode: valid type, size <= MAXFILE, block pointers in
 *      range, no block claimed twice (direct + indirect)
 *   3. bitmap == set of referenced blocks (+ metadata): reports both
 *      "in use but not marked" and "marked but leaked"
 *   4. directories: size multiple of 16, exactly one "." (-> self) and one
 *      ".." (-> real parent), entries point at allocated inodes, no dir has
 *      two parents
 *   5. reachability from root (orphans)
 *   6. link counts. xv6 convention: nlink = number of directory entries that
 *      name the inode, not counting "." entries (".." does count).
 * Exit status 0 = clean, 1 = errors.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "xv6fs.h"

#define META 0xffffffffu

static uint8_t *img;
static struct superblock sb;
static uint32_t datastart;
static int errs, warns;

#define ERR(...)  do { printf("ERROR: "); printf(__VA_ARGS__); putchar('\n'); errs++; } while (0)
#define WARN(...) do { printf("warning: "); printf(__VA_ARGS__); putchar('\n'); warns++; } while (0)

static uint8_t *blk(uint32_t n) { return img + (size_t)n * BSIZE; }
static struct dinode *iget(uint32_t i) { return (struct dinode *)blk(IBLOCK(i, sb)) + i % IPB; }

static uint32_t *owner;   /* per block: 0 free, META, or inode number */

static int claim(uint32_t b, uint32_t inum, const char *what) {
  if (b < datastart || b >= sb.size) {
    ERR("inode %u: %s block %u out of range [%u,%u)", inum, what, b, datastart, sb.size);
    return 0;
  }
  if (owner[b]) {
    ERR("block %u claimed twice: by inode %u and by inode %u (%s)", b, owner[b], inum, what);
    return 0;
  }
  owner[b] = inum;
  return 1;
}

static uint32_t bmap_ro(struct dinode *ip, uint32_t bn) {
  uint32_t b = 0;
  if (bn < NDIRECT) b = ip->addrs[bn];
  else if (bn - NDIRECT < NINDIRECT) {
    uint32_t ind = ip->addrs[NDIRECT];
    if (ind >= datastart && ind < sb.size) b = ((uint32_t *)blk(ind))[bn - NDIRECT];
  }
  return (b >= datastart && b < sb.size) ? b : 0;
}

/* Directory entry at byte offset off, or NULL if in a hole. Entries never
 * straddle blocks because 16 divides BSIZE. */
static struct dirent *dent(struct dinode *ip, uint32_t off) {
  uint32_t b = bmap_ro(ip, off / BSIZE);
  return b ? (struct dirent *)(blk(b) + off % BSIZE) : NULL;
}

static void namecpy(char *dst, const struct dirent *de) {
  memcpy(dst, de->name, DIRSIZ);
  dst[DIRSIZ] = 0;
}

int main(int argc, char **argv) {
  if (argc != 2) { fprintf(stderr, "usage: %s fs.img\n", argv[0]); return 2; }
  int fd = open(argv[1], O_RDONLY);
  if (fd < 0) { perror(argv[1]); return 2; }
  struct stat st;
  fstat(fd, &st);
  if (st.st_size < 2 * BSIZE) { fprintf(stderr, "image too small\n"); return 2; }
  img = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (img == MAP_FAILED) { perror("mmap"); return 2; }
  memcpy(&sb, blk(1), sizeof sb);

  /* ---- 1. superblock ---- */
  uint32_t nib = (sb.ninodes + IPB - 1) / IPB, nbm = (sb.size + BPB - 1) / BPB;
  if (sb.size == 0 || (uint64_t)sb.size * BSIZE > (uint64_t)st.st_size ||
      sb.nblocks >= sb.size || sb.ninodes < 2) {
    printf("FATAL: bad superblock (size=%u nblocks=%u ninodes=%u, file has %lld blocks)\n",
           sb.size, sb.nblocks, sb.ninodes, (long long)st.st_size / BSIZE);
    return 1;
  }
  datastart = sb.size - sb.nblocks;
  if (sb.logstart < 2 || sb.inodestart < sb.logstart + sb.nlog ||
      sb.inodestart + nib > sb.bmapstart || sb.bmapstart + nbm > datastart) {
    printf("FATAL: superblock regions overlap or exceed the data start (%u)\n", datastart);
    return 1;
  }
  if ((uint64_t)sb.size * BSIZE < (uint64_t)st.st_size)
    WARN("image file has %lld trailing bytes beyond sb.size", (long long)st.st_size - (long long)sb.size * BSIZE);

  owner = calloc(sb.size, sizeof *owner);
  for (uint32_t b = 0; b < datastart; b++) owner[b] = META;
  uint8_t *alloc = calloc(sb.ninodes, 1);
  uint8_t *reach = calloc(sb.ninodes, 1);
  uint32_t *refcnt = calloc(sb.ninodes, sizeof *refcnt);
  uint32_t *parent = calloc(sb.ninodes, sizeof *parent);
  uint32_t *dotdot = calloc(sb.ninodes, sizeof *dotdot);
  uint32_t *dirrefs = calloc(sb.ninodes, sizeof *dirrefs);
  uint32_t nfiles = 0, ndirs = 0, ndevs = 0;

  /* ---- 2. inodes & their blocks ---- */
  for (uint32_t i = 1; i < sb.ninodes; i++) {
    struct dinode *ip = iget(i);
    if (ip->type == 0) continue;
    if (ip->type < T_DIR || ip->type > T_DEV) { ERR("inode %u: invalid type %d", i, ip->type); continue; }
    alloc[i] = 1;
    if (ip->type == T_DIR) ndirs++; else if (ip->type == T_FILE) nfiles++; else ndevs++;
    if (ip->size > (uint32_t)MAXFILE * BSIZE) ERR("inode %u: size %u exceeds MAXFILE", i, ip->size);
    uint64_t nb = ((uint64_t)ip->size + BSIZE - 1) / BSIZE;
    for (uint32_t d = 0; d < NDIRECT; d++)
      if (ip->addrs[d]) {
        claim(ip->addrs[d], i, "direct");
        if (d >= nb) WARN("inode %u: direct block %u lies beyond EOF (size %u)", i, d, ip->size);
      }
    uint32_t ind = ip->addrs[NDIRECT];
    if (ind && claim(ind, i, "indirect")) {
      uint32_t *x = (uint32_t *)blk(ind);
      for (uint32_t j = 0; j < NINDIRECT; j++)
        if (x[j]) {
          claim(x[j], i, "indirect-data");
          if (NDIRECT + j >= nb) WARN("inode %u: indirect entry %u lies beyond EOF (size %u)", i, j, ip->size);
        }
    }
  }

  /* ---- 3. bitmap ---- */
  uint32_t used = 0, freeb = 0;
  for (uint32_t b = 0; b < sb.size; b++) {
    int bit = (blk(BBLOCK(b, sb))[(b % BPB) / 8] >> (b % 8)) & 1;
    if (owner[b] && !bit)
      ERR("block %u is %s but NOT marked in the bitmap", b,
          owner[b] == META ? "metadata" : "in use");
    else if (!owner[b] && bit)
      ERR("block %u is marked in the bitmap but unreferenced (leaked)", b);
    if (b >= datastart) { if (owner[b]) used++; else freeb++; }
  }

  /* ---- 4. directories ---- */
  if (!alloc[ROOTINO] || iget(ROOTINO)->type != T_DIR) ERR("root inode %d is not an allocated directory", ROOTINO);
  for (uint32_t i = 1; i < sb.ninodes; i++) {
    struct dinode *ip = iget(i);
    if (!alloc[i] || ip->type != T_DIR) continue;
    if (ip->size % sizeof(struct dirent)) ERR("dir %u: size %u not a multiple of %zu", i, ip->size, sizeof(struct dirent));
    int ndot = 0, nddot = 0;
    for (uint32_t off = 0; off + sizeof(struct dirent) <= ip->size; off += sizeof(struct dirent)) {
      struct dirent *de = dent(ip, off);
      if (!de || !de->inum) continue;
      char nm[DIRSIZ + 1];
      namecpy(nm, de);
      if (de->inum >= sb.ninodes || !alloc[de->inum]) {
        ERR("dir %u: entry '%s' -> unallocated/invalid inode %u", i, nm, de->inum);
        continue;
      }
      if (!strcmp(nm, ".")) {
        ndot++;
        if (de->inum != i) ERR("dir %u: '.' points to %u", i, de->inum);
        continue;
      }
      refcnt[de->inum]++;
      if (!strcmp(nm, "..")) {
        nddot++;
        dotdot[i] = de->inum;
        continue;
      }
      if (iget(de->inum)->type == T_DIR) {
        if (dirrefs[de->inum]++) ERR("directory %u has more than one parent (seen again in dir %u)", de->inum, i);
        parent[de->inum] = i;
      }
    }
    if (ndot != 1) ERR("dir %u: has %d '.' entries (want 1)", i, ndot);
    if (nddot != 1) ERR("dir %u: has %d '..' entries (want 1)", i, nddot);
  }
  for (uint32_t i = 1; i < sb.ninodes; i++) {
    if (!alloc[i] || iget(i)->type != T_DIR) continue;
    uint32_t want = (i == ROOTINO) ? ROOTINO : parent[i];
    if (i != ROOTINO && !dirrefs[i]) continue;      /* orphan, reported below */
    if (dotdot[i] != want) ERR("dir %u: '..' -> %u but real parent is %u", i, dotdot[i], want);
  }

  /* ---- 5. reachability ---- */
  uint32_t *stack = malloc(sb.ninodes * sizeof *stack);
  int sp = 0;
  if (alloc[ROOTINO]) { reach[ROOTINO] = 1; stack[sp++] = ROOTINO; }
  while (sp) {
    uint32_t d = stack[--sp];
    struct dinode *ip = iget(d);
    if (ip->type != T_DIR) continue;
    for (uint32_t off = 0; off + sizeof(struct dirent) <= ip->size; off += sizeof(struct dirent)) {
      struct dirent *de = dent(ip, off);
      if (!de || !de->inum || de->inum >= sb.ninodes || !alloc[de->inum]) continue;
      if (!strncmp(de->name, ".", DIRSIZ) || !strncmp(de->name, "..", DIRSIZ)) continue;
      if (!reach[de->inum]) { reach[de->inum] = 1; stack[sp++] = de->inum; }
    }
  }
  for (uint32_t i = 1; i < sb.ninodes; i++)
    if (alloc[i] && !reach[i]) ERR("inode %u (type %d, nlink %d) is allocated but unreachable from /", i, iget(i)->type, iget(i)->nlink);

  /* ---- 6. link counts ---- */
  for (uint32_t i = 1; i < sb.ninodes; i++) {
    if (!alloc[i]) continue;
    struct dinode *ip = iget(i);
    if (ip->nlink != (int)refcnt[i])
      ERR("inode %u: nlink is %d but %u directory entries reference it", i, ip->nlink, refcnt[i]);
  }

  printf("xv6fsck: %u files, %u dirs, %u devs; data blocks %u used / %u free of %u; %d error(s), %d warning(s)\n",
         nfiles, ndirs, ndevs, used, freeb, sb.nblocks, errs, warns);
  puts(errs ? "FILE SYSTEM HAS ERRORS" : "file system is consistent");
  return errs ? 1 : 0;
}
