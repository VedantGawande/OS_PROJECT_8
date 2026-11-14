/* mkimg -- minimal stand-in for xv6's mkfs: empty fs (root dir only).
 * usage: mkimg fs.img [nblocks=2000] [ninodes=200]
 * Layout matches xv6-public: [boot][super][log][inodes][bitmap][data]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xv6fs.h"

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s fs.img [nblocks] [ninodes]\n", argv[0]); return 2; }
  uint32_t size = argc > 2 ? atoi(argv[2]) : 2000;
  uint32_t ninodes = argc > 3 ? atoi(argv[3]) : 200;
  uint32_t nlog = 30, nib = ninodes / IPB + 1, nbm = size / BPB + 1;
  uint32_t nmeta = 2 + nlog + nib + nbm;
  uint8_t *img = calloc(size, BSIZE);

  struct superblock sb = { size, size - nmeta, ninodes, nlog, 2, 2 + nlog, 2 + nlog + nib };
  memcpy(img + BSIZE, &sb, sizeof sb);

  uint32_t rootblk = nmeta;                      /* first data block */
  struct dinode *root = (struct dinode *)(img + (size_t)IBLOCK(ROOTINO, sb) * BSIZE) + ROOTINO % IPB;
  root->type = T_DIR; root->nlink = 1; root->size = 2 * sizeof(struct dirent);
  root->addrs[0] = rootblk;
  struct dirent *de = (struct dirent *)(img + (size_t)rootblk * BSIZE);
  de[0].inum = ROOTINO; strcpy(de[0].name, ".");
  de[1].inum = ROOTINO; strcpy(de[1].name, "..");

  uint8_t *bm = img + (size_t)sb.bmapstart * BSIZE;
  for (uint32_t b = 0; b <= rootblk; b++) bm[b / 8] |= 1 << (b % 8);

  FILE *f = fopen(argv[1], "wb");
  if (!f || fwrite(img, BSIZE, size, f) != size) { perror(argv[1]); return 1; }
  fclose(f);
  printf("%s: %u blocks (%u meta, %u data), %u inodes\n", argv[1], size, nmeta, size - nmeta, ninodes);
  return 0;
}
