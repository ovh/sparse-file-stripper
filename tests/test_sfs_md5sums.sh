#!/bin/bash

set -e -o pipefail -x -u

BINDIR=${BINDIR:-"/tmp/sparse-file-stripper/build/bin"}
SFSZ_PARAMS=${SFSZ_PARAMS:-""}

# Setup
TESTDIR=$(mktemp -d)

function tear_down () {
    echo "Test tear down"
    rm -rf $TESTDIR
}

trap 'tear_down' EXIT

function compute_md5 () {
    md5sum $1 | awk '{print $1}'
}

# Test file backup/restore with uncommon boundaries
# Compare md5sums for three outputs:
# - one already pre-existing with exact same size as uncompressed data (datadst1)
# - another one pre-existing but bigger than uncompressed data (datadst2)
# - a last one created when uncompressing data (datadst3)
function run_test () {
    dd if=/dev/urandom of=$TESTDIR/chunk1 count=1024 bs=4096
    dd if=/dev/zero of=$TESTDIR/chunk2 count=1024 bs=4096
    dd if=/dev/zero of=$TESTDIR/chunk3 count=1 bs=714
    cat $TESTDIR/chunk2 $TESTDIR/chunk1 $TESTDIR/chunk2 $TESTDIR/chunk1 $TESTDIR/chunk2 $TESTDIR/chunk2 $TESTDIR/chunk1 $TESTDIR/chunk3 > $TESTDIR/data1
    dd if=/dev/urandom of=$TESTDIR/datadst1 bs=29360842 count=1
    dd if=/dev/urandom of=$TESTDIR/datadst2 bs=29360900 count=1
    dd if=$TESTDIR/datadst2 of=$TESTDIR/untouched skip=29360842 iflag=skip_bytes
    rm -rf $TESTDIR/datadst3
    witness=$(compute_md5 $TESTDIR/data1)
    witness2=$(compute_md5 $TESTDIR/untouched)
    $BINDIR/sfsz ${SFSZ_PARAMS} -b $((8192+4096)) $TESTDIR/data1 $TESTDIR/data1.cbz

    echo "Generated files"
    ls -alh $TESTDIR

    echo "Inflating to pre existing file matching size"
    $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/datadst1
    check=$(compute_md5 $TESTDIR/datadst1)
    if [[ "$check" != "$witness" ]];then
        echo "ERROR $TESTDIR/datadst1 and $TESTDIR/data1 md5 sums differ ($check != $witness)"
        false
    fi

    echo "Inflating to pre existing file not matching size"
    $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/datadst2
    check=$(compute_md5 $TESTDIR/datadst2)
    if [[ "$check" == "$witness" ]];then
        echo "ERROR $TESTDIR/datadst2 and $TESTDIR/data1 md5 sums should not be equal ($witness)"
        false
    fi

    echo "Checking untouched part of $TESTDIR/datadst2"
    dd if=$TESTDIR/datadst2 of=$TESTDIR/check_untouched skip=29360842 iflag=skip_bytes
    check=$(compute_md5 $TESTDIR/check_untouched)
    if [[ "$check" != "$witness2" ]];then
        echo "ERROR $TESTDIR/check_untouched and $TESTDIR/untouched md5 sums differ ($check != $witness2)"
        false
    fi

    echo "Inflating to non existing file"
    $BINDIR/sfsuz $TESTDIR/data1.cbz $TESTDIR/datadst3
    check=$(compute_md5 $TESTDIR/datadst3)
    if [[ "$check" != "$witness" ]];then
        echo "ERROR $TESTDIR/datadst3 and $TESTDIR/data1 md5 sums differ ($check != $witness)"
        false
    fi
    echo TEST OK
}

run_test
