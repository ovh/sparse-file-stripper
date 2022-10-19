#!/bin/bash

export SFSZ_PARAMS="-k $((1048576)) -r 1024"

$(dirname "${BASH_SOURCE[0]}")/test_sfs_md5sums.sh
