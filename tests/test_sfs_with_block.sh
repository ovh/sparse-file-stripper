#!/bin/bash

set -e -o pipefail -u

BINDIR=${BINDIR:-"/tmp/sparse-file-stripper/build/bin"}
SFS_ATOMIC_SIZE=${SFS_ATOMIC_SIZE:-""}
EXPECTED_ATOMIC_BLOCKS=${EXPECTED_ATOMIC_BLOCKS:-1}
SFSZ_PARAMS=${SFSZ_PARAMS:-""}
if [[ -n "$SFS_ATOMIC_SIZE" ]];then
    SFSZ_PARAMS="${SFSZ_PARAMS} -b ${SFS_ATOMIC_SIZE}"
fi
BLKDEV=${BLKDEV:-""}
FORCE_YES=${FORCE_YES:-"0"}

testdir=$(mktemp -d)

function tear_down () {
    rm -rf $testdir
}

function exit_on_err () {
    echo "last command: ${last_command:-unknown}"
    echo "ERROR line $LINENO: status $?"
    tear_down
    exit 1
}

current_command=''
trap 'last_command=$current_command; current_command=$BASH_COMMAND' DEBUG
trap exit_on_err ERR
trap tear_down EXIT

echo WARNING!!! This test will erase data on the block device you provide. So please make sure you do not care about losing data on it.

if [ -z "${BLKDEV}" ];then
    echo Provide block device to test backup/restore [/dev/loop42]
    read block_dev
    block_dev=${block_dev:-'/dev/loop42'}
else
    block_dev=$BLKDEV
fi

if ! [ -b ${block_dev} ];then
    echo "Block device ${block_dev} does not exist"
    exit 42
fi

if [ "${FORCE_YES}" -eq "0" ];then
    echo "${block_dev} will be erased. Proceed Y/N ? [N]"
    read answer
    answer=${answer:-'N'}
    if [[ "$answer" != "Y" ]] && [[ "$answer" != "y" ]];then
        echo "Aborting"
        exit 0
    fi
fi

size=$(blockdev --getsize64 ${block_dev})
# we limit the test size to 100MiB so that it does not take too long
if [[ $size -gt 104857600 ]];then
    echo "Limiting testing size to first 100MiB"
    size=104857600
fi

echo "Building source image"

src=${testdir}/src.img

dd if=/dev/urandom of=$src bs=$size count=1 iflag=fullblock

# Add some sparse areas

# Create some zero range areas in 10% of the total size chunks
sparse_chunk_size=$(echo "($size * 0.1) / 1" | bc)

# Sparse area 0-10%
dd if=/dev/zero of=$src bs=${sparse_chunk_size} count=1 iflag=fullblock conv=notrunc
seek_offset1=$(echo "${sparse_chunk_size} * 3" | bc)
# Sparse area 30-40%
dd if=/dev/zero of=$src bs=${sparse_chunk_size} seek=${seek_offset1} count=1 iflag=fullblock conv=notrunc oflag=seek_bytes
# Sparse area 80-90%
seek_offset2=$(echo "${sparse_chunk_size} * 8" | bc)
dd if=/dev/zero of=$src bs=${sparse_chunk_size} seek=${seek_offset2} count=1 iflag=fullblock conv=notrunc oflag=seek_bytes

echo "Source image prepared"
witness=$(md5sum $src | awk '{print $1}')
echo checksum witness $witness

echo "Dumping source to ${block_dev}"
cp $src ${block_dev}

echo "Block device source prepared"
function chksum () {
    dd if=${block_dev} bs=$size count=1 | md5sum | awk '{print $1}'
}

check=$(chksum)

if [[ "$witness" != "$check" ]];then
    echo "UNEXPECTED checksum on ${block_dev} after initial test img copy: $witness != $check"
    false
fi

echo "######################################################"
echo "OK: ${block_dev} checksum after image initial copy"
echo "######################################################"

backup=${testdir}/backup.img

echo "Creating backup"
${BINDIR}/sfsz ${SFSZ_PARAMS} ${src} $backup

check=$(chksum)
if [[ "$check" != "$witness" ]];then
    echo "UNEXPECTED checksum on source after backup: $witness != $check"
    false
fi

echo "######################################################"
echo "OK: ${block_dev} checksum after backup"
echo "######################################################"

atomic_blocks=$($BINDIR/sfs_stats $backup | grep -oP '^.*atomic_blocks=\K\d+(?=.*)$')

if [[ "${atomic_blocks}" != "${EXPECTED_ATOMIC_BLOCKS}" ]];then
    echo "ERROR: Unexpected number of atomic blocks: ${atomic_blocks} != ${EXPECTED_ATOMIC_BLOCKS}"
    false
fi

echo "######################################################"
echo "OK: expected number of atomic blocks for $backup"
echo "######################################################"

echo "Filling source with random data"
dd if=/dev/urandom of=${block_dev} bs=$size count=1 iflag=fullblock

check=$(chksum)
if [[ "$check" == "$witness" ]];then
    echo "Checksums should not be equal here: $witness == $check"
    false
fi

echo "######################################################"
echo "OK: ${block_dev} different checksum after random erase"
echo "######################################################"

echo 'Restoring backup'
${BINDIR}/sfsuz $backup ${block_dev}
check=$(chksum)
if [[ "$check" != "$witness" ]];then
    echo "UNEXPECTED checksum on ${block_dev} after restore: $witness != $check"
    false
fi

echo "######################################################"
echo "OK: ${block_dev} checksum after restore"
echo "######################################################"
