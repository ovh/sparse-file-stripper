#!/bin/bash

set -e -o pipefail -u

BINDIR=${BINDIR:-"/tmp/sparse-file-stripper/build/bin"}
SFS_ATOMIC_SIZE=${SFS_ATOMIC_SIZE:-""}
TESTSIZE=${TESTSIZE:-104857600}
EXPECTED_ATOMIC_BLOCKS=${EXPECTED_ATOMIC_BLOCKS:-1}
SFSZ_PARAMS=${SFSZ_PARAMS:-""}
if [[ -n "$SFS_ATOMIC_SIZE" ]];then
    SFSZ_PARAMS="${SFSZ_PARAMS} -b ${SFS_ATOMIC_SIZE}"
fi

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

echo "Building source image"

src=${testdir}/src.img

dd if=/dev/urandom of=$src bs=$TESTSIZE count=1 iflag=fullblock

# Add some sparse areas

# Create some zero range areas in 10% of the total size chunks
sparse_chunk_size=$(echo "($TESTSIZE * 0.1) / 1" | bc)

echo Sparse chunk size: ${sparse_chunk_size} bytes

# Sparse area 0-10%
dd if=/dev/zero of=$src bs=${sparse_chunk_size} count=1 iflag=fullblock conv=notrunc
seek_offset1=$(echo "${sparse_chunk_size} * 3" | bc)
# Sparse area 30-40%
dd if=/dev/zero of=$src bs=${sparse_chunk_size} seek=${seek_offset1} count=1 iflag=fullblock conv=notrunc oflag=seek_bytes
# Sparse area 80-90%
seek_offset2=$(echo "${sparse_chunk_size} * 8" | bc)
dd if=/dev/zero of=$src bs=${sparse_chunk_size} seek=${seek_offset2} count=1 iflag=fullblock conv=notrunc oflag=seek_bytes

echo "Source image prepared"

echo "Test directory $testdir listing"
ls -alh $testdir

function chksum () {
    md5sum $src | awk '{print $1}'
}

witness=$(chksum)

echo checksum witness $witness

backup=${testdir}/backup.img

echo "Creating backup"

cmd="${BINDIR}/sfsz ${SFSZ_PARAMS} ${src} $backup"
echo "SFSZ COMMAND: $cmd"
$cmd

check=$(chksum)
if [[ "$check" != "$witness" ]];then
    echo "UNEXPECTED checksum on source after backup: $witness != $check"
    false
fi

echo "######################################################"
echo "OK: ${src} checksum after backup"
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
dd if=/dev/urandom of=${src} bs=$TESTSIZE count=1 iflag=fullblock conv=notrunc

check=$(chksum)
if [[ "$check" == "$witness" ]];then
    echo "Checksums should not be equal here: $witness == $check"
    false
fi

echo "######################################################"
echo "OK: $src different checksum after random erase"
echo "######################################################"

echo 'Restoring backup'
${BINDIR}/sfsuz $backup ${src}
check=$(chksum)
if [[ "$check" != "$witness" ]];then
    echo "UNEXPECTED checksum on $src after restore: $witness != $check"
    false
fi

echo "######################################################"
echo "OK: ${src} checksum after restore"
echo "######################################################"
