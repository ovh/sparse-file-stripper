#!/bin/bash

export TESTSIZE=104856886
export SFS_ATOMIC_SIZE=32505856
export BINDIR=${BINDIR:-"/tmp/sparse-file-stripper/build/bin"}
export EXPECTED_ATOMIC_BLOCKS=3

$(dirname "${BASH_SOURCE[0]}")/test_sfs_with_file.sh