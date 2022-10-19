#!/bin/bash


export SFSZ_PARAMS="-r 1024 -k 3145728"
export EXPECTED_ATOMIC_BLOCKS=34

$(dirname "${BASH_SOURCE[0]}")/test_sfs_with_file.sh
