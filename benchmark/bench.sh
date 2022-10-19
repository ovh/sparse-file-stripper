#!/bin/bash

set -e -o pipefail -E -u

BINDIR=${BINDIR:-"/tmp/sparse-file-stripper/build/bin"}

garbage_file=$(mktemp -p /tmp)

trap "rm -f ${garbage_file}" ERR SIGINT EXIT SIGTERM

echo 'File reference: [/tmp/image1.raw]'
read reference
reference=${reference:-'/tmp/image1.raw'}
if ! [ -f "$reference" ];then
    echo $reference does not exist
    exit 1
fi

echo 'Backup source: [/tmp/image1.src]'
read src
src=${src:-'/tmp/image1.src'}
if ! [ -f "$src" -o -b "$src" ];then
    echo $src does not exist
    exit 1
fi

echo 'Backup destination: [/tmp/backup.img]'
read dst
dst=${dst:-'/tmp/backup.img'}

echo 'Results directory: [/tmp/results]'
read results
results=${results:-'/tmp/results'}

if [ -z "$results" -o -f "$results" ];then
    echo result directory "$results" already exists
    exit 1
fi

echo 'Iterations: [20]'
read max_iter
max_iter=${max_iter:-20}

# dirname $results expected to exist, if not, exit
mkdir $results

if [ ! -f $src ];then
    echo test > $src
fi

echo checking fallocate support of $src
falloc_method_enabled=1
fallocate -p -l 512 -o 0 $src || falloc_method_enabled=0

echo init $src from $reference
cat $reference > $src

echo checksum witness
witness=$(md5sum $reference | awk '{print $1}' | tee $results/witness)

check=$(md5sum $src | awk '{print $1}')
if [[ "$check" != "$witness" ]];then
    echo ERROR: unable to init $src correctly
    exit 1
fi

random_chunk_size=$(ls -al ${reference} | awk '{print $5}')
random_count=$(python -c 'import sys; s=int(sys.argv[1]); print(int(s/2**20)) if ((s >= 2**20) and (s % 2**20 == 0)) else sys.exit(1)' ${random_chunk_size})

echo building random garbage for consistency checks
dd if=/dev/urandom of=${garbage_file} bs=1048576 count=${random_count}

new_size=$(ls -al ${garbage_file} | awk '{print $5}')
if [[ "$random_chunk_size" != "$new_size" ]];then
    echo "Error while building random garbage ($random_chunk_size != $new_size)"
    exit 1
fi

echo '############################################################################'
echo "Benchmark: compress/inflate from/to $src to/from $dst, results directory $results, iterations: ${max_iter}"

declare -A compress_cmds
compress_cmds=(
    ["qcow2-u"]="qemu-img convert -O qcow2 -f raw $src $dst"
    ["qcow2-c"]="qemu-img convert -O qcow2 -f raw -c $src $dst"
    ["sfs"]="$BINDIR/sfsz $src $dst"
    ["gzip"]="gzip --fast -c $src > $dst"
    ["xz"]="xz --fast -c $src > $dst"
    ["lz4"]="lz4 -z -f $src $dst"
    # NOTE: cat command below is actually not useless: it allows us to make the pigz/pixz work correctly while
    # handling block devices. Otherwise is fails with some error like 'not a regular file' (why should it care anyway...)
    # or Error decoding stream footer for pixz
    ["pixz"]="cat $src | pixz -0 -t -o $dst"
    ["pigz"]="cat $src | pigz --fast -c - > $dst"
    ["sfs+lz4"]="$BINDIR/sfsz $src - | lz4 -f -z - $dst"
    ["sfs+xz"]="$BINDIR/sfsz $src - | xz -0 -c > $dst"
    ["sfs+gzip"]="$BINDIR/sfsz $src - | gzip --fast -c > $dst"
    ["sfs+pixz"]="$BINDIR/sfsz $src - | pixz -0 -t -c -o $dst"
    ["sfs+pigz"]="$BINDIR/sfsz $src - | pigz --fast -c > $dst"
    ["raw"]="cat $src > $dst"
)

declare -A inflate_cmds
inflate_cmds=(
    ["qcow2-u"]="qemu-img convert -O raw -f qcow2 $dst $src"
    ["qcow2-c"]="qemu-img convert -O raw -f qcow2 $dst $src"
    ["sfs"]="$BINDIR/sfsuz $dst $src"
    ["gzip"]="gzip -d -c $dst > $src"
    ["xz"]="xz -d -c $dst > $src"
    # No choice but to explicitely disable sparse for lz4, it has the same issue as dd conv=sparse
    # see note below. Two ways to achieve the same result, more robust accross lz4 versions selected is to redirect stdout
    # implicit --no-sparse, from the man
    # ["lz4"]="lz4 -f -d --no-sparse $dst $src"
    ["lz4"]="lz4 -f -d $dst > $src"
    ["pixz"]="pixz -d -t -i $dst -o $src"
    ["pigz"]="pigz -d -c $dst > $src"
    ["sfs+lz4"]="lz4 -d -c $dst | $BINDIR/sfsuz - $src"
    ["sfs+xz"]="xz -d -c $dst | $BINDIR/sfsuz - $src"
    ["sfs+gzip"]="gzip -d -c $dst | $BINDIR/sfsuz - $src"
    ["sfs+pixz"]="pixz -d -t -c -i $dst | $BINDIR/sfsuz - $src"
    ["sfs+pigz"]="pigz -d -c $dst | $BINDIR/sfsuz - $src"
    ["raw"]="cat $dst > $src"
)

if [[ "${falloc_method_enabled}" == "1" ]];then
    echo "dd conv=sparse compress/inflate enabled"
    compress_cmds["dd+sparse"]="dd conv=sparse if=$src of=$dst"

    echo "lz4_sparse compress/inflate enabled"
    compress_cmds["lz4_sparse"]="lz4 -z -f --sparse $src $dst"

    # NOTE(rg): the consistency checks with garbage data shows that dd conv=sparse does not work correctly
    # when restoring data: everything happens as if the sparse sections were just skipped on destination
    # instead of being explicitely written.
    # Assuming image1.raw is 4508876800 bytes big as an example:
    # $> dd if=/dev/urandom of=image2.raw bs=1048576 count=4300
    # $> dd if=image1.raw of=image2.raw conv=notrunc,sparse
    # $> md5sum image*raw -> md5sums are not equal as soon as there are sparse regions in image1.raw
    # So we change the dd sparse restore method
    # Same issue with lz4 sparse
    inflate_cmds["dd+sparse"]="fallocate -p -l ${random_chunk_size} -o 0 $src && dd conv=sparse if=$dst of=$src"
    inflate_cmds["lz4_sparse"]="fallocate -p -l ${random_chunk_size} -o 0 $src && lz4 -f -d --sparse $dst $src"
fi

for i in $(seq 1 ${max_iter});do
    echo '##################################################'
    echo ITER $i / ${max_iter}
    for algo in "${!compress_cmds[@]}"; do
        echo '##################################################'
        echo ALGO $algo
        suffix=$algo
        format_cmd=${compress_cmds[$algo]}
        inflate_cmd=${inflate_cmds[$algo]}
        echo format $format_cmd
        # Important to flush caches for example for ext4 or nfs tests as we do not want
        # to measure cache optimizations caused by repeated read/write ops
        echo 3 > /proc/sys/vm/drop_caches
        { time eval ${format_cmd} ; } 2>>$results/$suffix.compress
        echo formatted volume
        ls -al $dst >> $results/$suffix.volume

        echo "garbage for consistency results"
        cat ${garbage_file} > $src

        echo inflate $inflate_cmd
        echo 3 > /proc/sys/vm/drop_caches
        { time eval ${inflate_cmd} ; } 2>>$results/$suffix.inflate
        echo checksum
        check=$(md5sum $src | awk '{print $1}')
        if [[ "$check" != "$witness" ]];then
            echo Bad checksum $check != $witness
            false
        fi
    done
done

parse_script=$(dirname "${BASH_SOURCE[0]}")/result_parsing/parse_results.py
$parse_script $results
