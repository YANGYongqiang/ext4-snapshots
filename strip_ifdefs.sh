#!/bin/sh
# strip fake ifdefs from ext4-snapshots branch

BASE=v2.6.37
PATCH=ext4_snapshots.patch

# re-create the strip_ifdefs branch from current branch
(git branch | grep strip_ifdefs) && (git branch -d strip_ifdefs || exit 1)
git checkout -b strip_ifdefs || exit 1

make clean SUBDIRS=fs/ext4
rm -f fs/ext4/*.tmp

echo "stripping fake snapshot ifdefs from ext4 files..."
# strip all SNAPSHOT ifdefs from C files
for f in $( ls fs/ext4/*.c ) ; do
	./strip_ifdefs $f $f.tmp snapshot y || exit 1
	mv -f $f.tmp $f || exit 1
done
# strip all SNAPSHOT ifdefs from h files
for f in fs/ext4/Kconfig $( ls fs/ext4/*.h ) ; do
	./strip_ifdefs $f $f.tmp snapshot y || exit 1
	mv -f $f.tmp $f || exit 1
done

git commit -a -m "stripped fake SNAPSHOT ifdefs"

echo "ext4 files changed by snapshots patch:"
git diff --stat $BASE fs/ext4

# create one big snapshots patch and run it through checkpatch
echo "checking ext4 snapshots patch..."
git diff $BASE fs/ext4 > $PATCH
./scripts/checkpatch.pl $PATCH | tee $PATCH.check | tail


