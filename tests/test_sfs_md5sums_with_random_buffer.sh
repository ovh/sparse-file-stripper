#!/bin/bash

export SFSZ_PARAMS="-r 1024"

$(dirname "${BASH_SOURCE[0]}")/test_sfs_md5sums.sh
