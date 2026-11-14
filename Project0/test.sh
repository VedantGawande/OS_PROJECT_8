#!/bin/bash
# Linux-side test of xv6fuse. Usage: ./test.sh [path/to/xv6/fs.img]
# With no argument a fresh image is made with ./mkimg. With an xv6 fs.img
# (built by `make fs.img` in xv6) a COPY is used, so the original is untouched.
cd "$(dirname "$0")" || exit 1
make -s || exit 1

IMG=/tmp/xv6fuse_test.img
if [ -n "$1" ]; then cp "$1" "$IMG"; else ./mkimg "$IMG" 2000 >/dev/null; fi
MNT=$(mktemp -d)
T=$(mktemp -d)
pass=0; failn=0
ok()   { pass=$((pass+1)); echo "  ok   $1"; }
bad()  { failn=$((failn+1)); echo "  FAIL $1"; }
check(){ if eval "$2" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }
cleanup(){ fusermount3 -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null; rmdir "$MNT" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT

echo "== initial fsck"
./xv6fsck "$IMG" || { echo "initial image is inconsistent"; exit 1; }
./xv6fuse "$IMG" "$MNT" || { echo "mount failed"; exit 1; }
sleep 0.5
FREE0=$(stat -f -c %f "$MNT")

echo "== Linux -> xv6: files of many sizes (direct/indirect boundaries)"
for sz in 0 1 100 511 512 513 6143 6144 6145 6657 40000 71680; do
  head -c $sz /dev/urandom > "$T/f$sz"
  cp "$T/f$sz" "$MNT/f$sz" && cmp -s "$T/f$sz" "$MNT/f$sz" && ok "size $sz roundtrip" || bad "size $sz roundtrip"
done
check "file > MAXFILE (71681) is refused" "! head -c 71681 /dev/zero > $MNT/toobig"
rm -f "$MNT/toobig"

echo "== directories"
check "mkdir"            "mkdir $MNT/d1 && mkdir $MNT/d1/d2"
check "cp into subdir"   "cp $T/f40000 $MNT/d1/d2/x && cmp $T/f40000 $MNT/d1/d2/x"
check "ls shows entries" "ls $MNT/d1/d2 | grep -qx x"
check "name > 14 refused" "! touch $MNT/0123456789abcde"
check "mkdir duplicate refused" "! mkdir $MNT/d1"
check "rmdir non-empty refused" "! rmdir $MNT/d1"
check "many entries in a dir (grows dir past 1 block)" \
  "for i in \$(seq 1 60); do touch $MNT/d1/e\$i; done; [ \$(ls $MNT/d1 | wc -l) -eq 61 ]"
check "remove them again" "for i in \$(seq 1 60); do rm $MNT/d1/e\$i; done; [ \$(ls $MNT/d1 | wc -l) -eq 1 ]"

echo "== write / truncate"
head -c 20000 /dev/urandom > "$T/a"; cp "$T/a" "$MNT/a"
check "overwrite in middle"  "dd if=/dev/zero of=$MNT/a bs=1 seek=7000 count=300 conv=notrunc 2>/dev/null; dd if=/dev/zero of=$T/a bs=1 seek=7000 count=300 conv=notrunc 2>/dev/null; cmp $T/a $MNT/a"
check "append"               "echo hello >> $MNT/a; echo hello >> $T/a; cmp $T/a $MNT/a"
check "truncate shrink (into direct)" "truncate -s 1000 $MNT/a; truncate -s 1000 $T/a; cmp $T/a $MNT/a"
check "grow after shrink reads zeros" "truncate -s 3000 $MNT/a; truncate -s 3000 $T/a; cmp $T/a $MNT/a"
check "truncate to 0"        "truncate -s 0 $MNT/a; [ \$(stat -c %s $MNT/a) -eq 0 ]"
check "sparse write past EOF" "printf X | dd of=$MNT/sp bs=1 seek=30000 2>/dev/null; printf X | dd of=$T/sp bs=1 seek=30000 2>/dev/null; cmp $T/sp $MNT/sp"
check "O_TRUNC via redirect" "echo new > $MNT/f100; [ \"\$(cat $MNT/f100)\" = new ]"

echo "== unlink frees blocks"
check "rm files" "rm $MNT/f* $MNT/a $MNT/sp"
check "rm -r subtree" "rm -r $MNT/d1"
FREE1=$(stat -f -c %f "$MNT")
[ "$FREE0" = "$FREE1" ] && ok "free blocks back to $FREE0" || bad "free blocks $FREE0 -> $FREE1 (leak!)"

echo "== fill the disk, then recover"
n=0; while dd if=/dev/urandom of="$MNT/fill$n" bs=4096 count=17 2>/dev/null; do n=$((n+1)); [ $n -gt 200 ] && break; done
check "hit ENOSPC (or inode limit)" "[ $n -gt 0 ]"
rm -f "$MNT"/fill*
FREE2=$(stat -f -c %f "$MNT")
[ "$FREE0" = "$FREE2" ] && ok "free blocks recovered after ENOSPC" || bad "free blocks $FREE0 -> $FREE2 after ENOSPC"

echo "== leave some content for the xv6 side"
mkdir "$MNT/docs"
echo "hello from linux" > "$MNT/hello.txt"
head -c 60000 /dev/urandom | base64 -w 60 | head -c 60000 > "$MNT/docs/big.txt"
md5sum "$MNT/docs/big.txt" | tee /tmp/xv6fuse_big.md5

fusermount3 -u "$MNT"; sleep 0.3
echo "== final fsck"
./xv6fsck "$IMG" && ok "fsck clean" || bad "fsck clean"
echo "== $pass passed, $failn failed"
[ $failn -eq 0 ]
