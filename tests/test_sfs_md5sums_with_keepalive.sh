#!/bin/bash

export SFSZ_PARAMS="-k $((1048576))"

$(dirname "${BASH_SOURCE[0]}")/test_sfs_md5sums.sh