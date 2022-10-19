#!/bin/bash

BINDIR=${BINDIR:-"/tmp/sparse-file-stripper/build/bin"}
TESTDIR=$(mktemp -d --suffix=.fuzz)

trap "rm -rf $TESTDIR" EXIT

dd if=/dev/urandom of=$TESTDIR/chunk1 bs=4096 count=1
dd if=/dev/zero of=$TESTDIR/chunk2 bs=4096 count=1

cat $TESTDIR/chunk1 $TESTDIR/chunk2 $TESTDIR/chunk1 $TESTDIR/chunk2 > $TESTDIR/data1

valgrind $BINDIR/sfsz $TESTDIR/data1 $TESTDIR/data1.cbz

# 8192 bytes len once compressed in our example
data_len=$(echo $(od -An -N 8 -t u8 -j 8 $TESTDIR/data1.cbz))

if [[ "${data_len}" != "8192" ]];then
    echo "Unexpected crafted data len (${data_len} != 8192)"
    exit 1
fi

echo "Original data length: $data_len"

cp $TESTDIR/data1.cbz $TESTDIR/data1.cbz.clean

function reset_data1 () {
    cp $TESTDIR/data1.cbz.clean $TESTDIR/data1.cbz
}

echo "Craft 1: updating data len out of boundaries"

printf "0: %.16x" 1800000 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1

dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8 oflag=seek_bytes
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

# Make it fall in the middle of offsets array
echo "Craft 2: fall in the middle of array (right after the first byte)"

printf "0: %.16x" 8193 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1

dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8 oflag=seek_bytes

valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

echo "Craft 3: fall in the middle of array"
printf "0: %.16x" 8208 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1

dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8 oflag=seek_bytes

valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

echo "Craft 4: tamper with offsets instead (playing with offset 1, non sparse closure)"

echo "Craft 4-1"

printf "0: %.16x" 8192 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8 oflag=seek_bytes

reset_data1

echo "Craft 4-2"

printf "0: %.16x" 16385 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8224 oflag=seek_bytes
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

echo "Craft 4-3"

printf "0: %.16x" 0 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8224 oflag=seek_bytes
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

echo "Craft 5: playing with offset 0, non sparse opening, should be 0 by convention"

printf "0: %.16x" 12 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8216 oflag=seek_bytes
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

echo "Craft 6: playing with offset 2, second non sparse region opening"

echo "Craft 6-1"

printf "0: %.16x" 0 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8232 oflag=seek_bytes
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

echo "Craft 6-2"
# Will work it's just that data2 will end up being equal to cat chunk1 chunk2 chunk2 chunk1 instead of original chunk1 chunk2 chunk1 chunk2

printf "0: %.16x" 8192 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8232 oflag=seek_bytes
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

cat $TESTDIR/chunk1 $TESTDIR/chunk2 $TESTDIR/chunk2 $TESTDIR/chunk1 > $TESTDIR/data3

check1=$(md5sum $TESTDIR/data2 | awk '{print $1}')
check2=$(md5sum $TESTDIR/data3 | awk '{print $1}')

if [[ "$check1" != "$check2" ]];then
    echo "ERROR: unexpected checksum for inflated tampered data"
    exit 1
fi

reset_data1

echo "Craft 6-3"

printf "0: %.16x" $((8192+4096)) |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8232 oflag=seek_bytes
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

echo "Craft 7: update both second offset and the final footer total_read long to bypass triggered unconsistency"

printf "0: %.16x" $((8192+4096)) |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8232 oflag=seek_bytes
printf "0: %.16x" 20480 |  xxd -r -g0 > $TESTDIR/tmp1
# Convert long to little endian binary format
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/tmp1 $TESTDIR/craft1
dd if=$TESTDIR/craft1 of=$TESTDIR/data1.cbz conv=notrunc seek=8256 oflag=seek_bytes
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

reset_data1

echo "Craft 8: fake random buffer byte size, announcing random buffer size bytes when actually switched off"
# echo "0: 01" | xxd -r -g0 > $TESTDIR/random_on
# echo "0: 00" | xxd -r -g0 > $TESTDIR/random_off
printf "0: %.16x" 1024 |  xxd -r -g0 > $TESTDIR/random_size_bytes_rev
objcopy -I binary -O binary --reverse-bytes=8 $TESTDIR/random_size_bytes_rev $TESTDIR/random_size_bytes

dd if=$TESTDIR/random_size_bytes of=$TESTDIR/data1.cbz conv=notrunc
valgrind $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/data2

# Random buffer size bytes
# /usr/bin/od -An -N 8 -t u8 -j 0

# First atomic block useful data size
# /usr/bin/od -An -N 8 -t u8 -j 8 data1.cbz
# 8192

# First atomic block array size
# /usr/bin/od -An -N 8 -t u8 -j $((8192+8+8)) data1.cbz
# 4 (2 non sparse block boundaries, relative offsets)
# [0, 4096, 4096, 4096]

# Total read
# /usr/bin/od -An -N 8 -t u8 -j $((8248+8)) data1.cbz
# 16384

# Total written
# /usr/bin/od -An -N 8 -t u8 -j 8264 data1.cbz
# 8280 (8 + 8 + 8192 + 8 + 4 * 8 + 8 = final footer delim -1L (ffff ffff ffff ffff, see sfsz.c) + 32)

# ratio, yet to be parsed correctly as a double
#/usr/bin/od -An -N 8 -f -j 8272 data1.cbz

# Number of atomic blocs
# /usr/bin/od -An -N 8 -t u8 -j 8280 data1.cbz
