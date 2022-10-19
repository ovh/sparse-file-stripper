/* Copyright 2022 OVHcloud
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sfs.h>

#define FIVE_GIB  (long) (5 * pow(2, 30))
#define MAX_RANDOM_BUFFER_SIZE (unsigned int) 10485760

void print_usage() {
    // The atomic_block_size_bytes can be adapted, depending on the target available memory.
    // It can also be seen as some sort of 'keepalive' when streaming to some endpoint
    // with a read timeout, so as to avoid too long 'silences' due to big sparse regions
    // The -k option can be seen as some sort of 'keepalive' when streaming
    // to counter too long silences due to big sparse regions
    // -r is to activate an additional random buffer in every atomic block with the specified size.
    // The random size is expected to be provided in bytes. It will be divided by sizeof(int) and floored.
    // We do not need a strong and secure random generator for this. The purpose is to prevent any compression tool further in the workflow from
    // cancelling the -k effect (due to empty atomic blocks pattern being caught)
    fprintf(stderr, "sfsz [-b atomic_block_size_bytes] [-k read_bytes_keepalive] [-r random_size_bytes] src_path dst_path\n");
}


int flush_block(void* buffer, size_t buf_offset, sfs_footer_t* footerp,
                 FILE *dfp, size_t meta_idx, size_t* data_boundaries,
                 size_t closure_offset, size_t random_size, int* random_buf) {
    size_t written;
    int i;

    // Push next block size (not counting the additional random if any)
    written = fwrite(&buf_offset, sizeof(size_t), 1, dfp);
    if(written != 1) {
        fprintf(stderr, "Write next block size error\n");
        return 1;
    }
    footerp->written += sizeof(size_t);

    // Push random
    if(random_buf != NULL)
    {
        for(i=0; i<random_size; i++){
            random_buf[i] = rand();
        }
        //fprintf(stderr, "Block random %d\n", random);
        written = fwrite((void *) random_buf, sizeof(int), random_size, dfp);
        if(written != random_size) {
            fprintf(stderr, "Write random in next block error\n");
            return 1;
        }
        footerp->written += sizeof(int) * random_size;
    }

    // Push block data
    written = fwrite((void *) buffer, 1, buf_offset, dfp);
    if(written != buf_offset) {
        fprintf(stderr, "Unable to write buffer correctly\n");
        return 1;
    }
    footerp->written += buf_offset;

    // Close the data range if we were in copy mode, i.e if meta_idx % 2 != 0
    if(meta_idx % 2 != 0) {
        data_boundaries[meta_idx] = closure_offset;
        meta_idx++;
    }

    //fprintf(stderr, "Flushing block with %li bytes, meta_idx %li, closure offset %li\n", buf_offset, meta_idx, closure_offset);

    /* else {
        // We were in sparse mode, so all data cluster were closed
    } */

    // Push offset array size (unit = number of long = bytes_size / 8)
    written = fwrite(&meta_idx, sizeof(size_t), 1, dfp);
    if(written != 1) {
        fprintf(stderr, "Write offsets size error\n");
        return 1;
    }

    footerp->written += sizeof(size_t);

    // Push offsets
    written = fwrite((void *) data_boundaries, sizeof(size_t), meta_idx, dfp);
    if(written != meta_idx) {
        fprintf(stderr, "Write meta error\n");
        return 1;
    }

    footerp->written += meta_idx * sizeof(size_t);
    return 0;
}


void clean_all(FILE *sfp, FILE *dfp, char *buffer, size_t *data_boundaries, int* random_buf) {
    close_all_files(2, sfp, dfp);
    free_all_mem(3, (void *) buffer, (void *) data_boundaries, (void *) random_buf);
}


int main(int argc, char *argv[])
{
    int c;
    // We start assuming the beginning of the file is not sparse
    unsigned int sparse_on = 0;
    unsigned int copy = 0;
    unsigned int force_buffer_flush = 0;
    size_t *data_boundaries = NULL;
    // Storing relative offsets instead of the absolute ones will probably be more perf
    // when calling fseek from seek_cur
    size_t relative_offset = 0;
    char zeros[BLK_SIZE];
    char src[BLK_SIZE];
    //Default stack size -> about 8MiB, our buffer won't fit in there.
    char* buffer = NULL;
    size_t meta_idx = 0, atomic_blocks = 0;
    size_t rb, written;
    size_t buf_offset = 0;
    size_t data_cluster_nb = 0;
    /* Default structure block size: this gives
     * the size of blocks to be bufferized in memory and processed
     * as a whole when downloading. Do not choose it big if your target
     * has not much memory */
    size_t atomic_block_size = 268435456;
    size_t read_bytes_keepalive = 0;
    size_t read_since_last_flush = 0;
    size_t random_size = 0, random_size_bytes = 0;
    int *random_buf = NULL;
    char *sfilename;
    char *dfilename;
    FILE *sfp = NULL;
    FILE *dfp = NULL;
    sfs_footer_t footer;
    footer.read = 0;
    footer.written = 0;
    footer.ratio = 0;
    footer.atomic_blocks = 0;
    size_t meta_len = 0, extend_meta = 0, meta_max_idx = 0;

    // We do not need a strong random generator, so we do not
    // lose time initializing the random seed. Besides we want
    // a repeatable process so the seed needs to stay the same
    srand(1);

    while ((c = getopt(argc, argv, ":b:k:r:")) != -1) {
        switch (c) {
            case 'c':
                random_size_bytes = (size_t) atol(optarg);
                if(random_size_bytes < sizeof(int))
                    fprintf(
                        stderr,
                        "WARNING: random buffer size must be greater than %li to take effect. Ignoring\n",
                        sizeof(int)
                    );
                if(random_size_bytes > MAX_RANDOM_BUFFER_SIZE) {
                    fprintf(stderr, "Random buffer size must be lower than %u bytes.\n", MAX_RANDOM_BUFFER_SIZE);
                    DIE("Bad random size\n");
                }
                random_size = random_size_bytes / sizeof(int);
                random_size_bytes = random_size * sizeof(int);
                fprintf(stderr, "Random buffer size (bytes): %li\n", random_size_bytes);
                break;
            case 'k':
                read_bytes_keepalive = (size_t) atol(optarg);
                break;
            case 'b':
                atomic_block_size = (size_t) atol(optarg);
                if(atomic_block_size % BLK_SIZE != 0)
                    DIE("Atomic block size must be a multiple of 4096 bytes");
                if(atomic_block_size > 4294967296 || atomic_block_size == 0)
                    DIE("Atomic block size must be greater than 0 and lower than 4294967296 bytes (4 GiB)\n");
                fprintf(stderr, "Custom atomic block size %li\n", atomic_block_size);
                break;
            case '?':
                print_usage();
                fprintf(stderr, "Unexpected argument -%c\n", optopt);
                exit(EXIT_FAILURE);
            case ':':
                print_usage();
                fprintf(stderr, "Option %c requires a value\n", optopt);
        }
    }

    // Positional arguments
    if(argv[optind] == NULL || argv[optind+1] == NULL) {
        print_usage();
        DIE("Missing mandatory param\n");
    }

    sfilename = argv[optind];
    dfilename = argv[optind+1];

    if(strcmp(sfilename, "-") == 0) {
        sfp = freopen(NULL, "rb", stdin);
        if(sfp == NULL) {
            clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
            DIE("Unable to reopen stdin in binary mode\n");
        }
    }
    else {
        sfp = fopen(sfilename, "rb");
        if(sfp == NULL) {
            clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
            DIE("Unable to open source file for reading\n");
        }
    }

    if(strcmp(dfilename, "-") == 0) {
        dfp = freopen(NULL, "wb", stdout);
        if(dfp == NULL) {
            clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
            DIE("Unable to reopen stdout in binary mode\n");
        }
    }
    else {
        dfp = fopen(dfilename, "wb");
        if(dfp == NULL) {
            clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
            DIE("Unable to open destination file for writing\n");
        }
    }

    // Allocate random buffer if needed
    if(random_size > 0){
        fprintf(stderr, "Random buffers activated!\n");
        random_buf = (int *) malloc(random_size_bytes);
        if(random_buf == NULL) {
            clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
            DIE("Unable to allocate random buffer\n");
        }
    }

    // Prepend the random_size_bytes value in the output for the sfsuz to know how to inflate the file later
    written = fwrite(&random_size_bytes, sizeof(size_t), 1, dfp);
    if(written != 1) {
        clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
        DIE("Unable to write to destination\n");
    }
    footer.written += sizeof(size_t);

    buffer = malloc(atomic_block_size);
    if(buffer == NULL) {
        fprintf(stderr, "Unable to allocate buffer size correctly (%li required). "
                "Decrease the block size.\n", atomic_block_size);
        clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
        exit(1);
    }

    memset(zeros, 0, BLK_SIZE);

    // In the worst case scenario, there will be atomic_block_size/BLK_SIZE + 2 boundaries,
    // if data and sparse regions are all 1 block long. +2 is if we actually start with a sparse region
    // The last bool is to make the max idx even, so that realloc activates correctly below if the
    // max size computed here was anyhow wrong
    extend_meta = (atomic_block_size / BLK_SIZE + 1) * 2;
    extend_meta *= sizeof(size_t);
    fprintf(stderr, "Estimated boundary array max size in bytes: %li\n", extend_meta);

    data_boundaries = malloc(extend_meta);
    if(data_boundaries == NULL) {
        clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
        DIE("Unable to allocate memory for data_boundaries. Try decreasing atomic block size.\n");
    }
    meta_len += extend_meta;
    meta_max_idx = meta_len / sizeof(size_t);
    assert( meta_max_idx % 2 == 0);
    // By convention, we start with sparse_mode off.
    // For clarity, we explicitely set the first data_boundaries item to 0 as the first data offset
    // (even if we could implictely skip it)
    data_boundaries[0] = 0;
    meta_idx++;
    fprintf(stderr, "Start reading\n");
    while ((rb = fread((void *) src, 1, BLK_SIZE, sfp)) > 0) {
        read_since_last_flush += rb;
        if(rb < BLK_SIZE || ((read_bytes_keepalive > 0) && (read_since_last_flush >= read_bytes_keepalive))) {
            if(rb < BLK_SIZE)
                fprintf(stderr, "Less than %d bytes read (%li bytes), unaligned so not skipping data\n",
                        BLK_SIZE, rb);
            else
                fprintf(stderr, "More than %li bytes read since last flush (%li bytes read). Forcing copy and flush (keepalive safety)\n",
                        read_bytes_keepalive, read_since_last_flush);
            copy = 1;
            force_buffer_flush = 1;
        }
        else {
            force_buffer_flush = 0;
            if(memcmp(src, zeros, BLK_SIZE) != 0) {
                copy = 1;
            }
            else {
                copy = 0;
            }
        }

        if(!copy) {
            if(!sparse_on) {
                if(meta_idx == meta_max_idx-1) {

                    // This section should normally be dead code, if the first upper boundary computed above is correct
                    // we have reached the end (1 slot left for us) of the data_boundaries,
                    // we need to realloc some space
                    fprintf(stderr, "Data_boundaries memory needs extension. Etending by %li bytes\n", extend_meta);
                    meta_len += extend_meta;
                    meta_max_idx = meta_len / sizeof(size_t);

                    data_boundaries = realloc(
                        data_boundaries, meta_len);
                    if(data_boundaries == NULL) {
                        clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
                        DIE("Unable to extend meta. Memory allocation error. Try decreasing atomic block size.\n");
                    }
                    fprintf(stderr, "data_boundaries size is now %li bytes\n",
                            meta_len);
                }
                data_boundaries[meta_idx] = relative_offset; // End a data range, start a new sparse range
                relative_offset = 0;
                meta_idx++;
                sparse_on = 1;
            }
            relative_offset += rb;
        }
        else {
            if(sparse_on) {
                sparse_on = 0;
                // If we are on a copy case, then we are certain meta_idx % 2 == 0 and
                // meta_idx < meta_max_idx-1. Thus we do not need to realloc
                data_boundaries[meta_idx] = relative_offset; // Start a new data range
                relative_offset = 0;
                meta_idx++;
            }
            memcpy(buffer+buf_offset, src, rb);
            buf_offset += rb;
            relative_offset += rb;
            if(force_buffer_flush || buf_offset == atomic_block_size) {

                assert(meta_idx % 2 == 1);

                if(flush_block(buffer, buf_offset, &footer, dfp, meta_idx,
                               data_boundaries, relative_offset, random_size, random_buf)) {
                    clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
                    DIE("Flush block error\n");
                }

                // Increment data cluster number for stats
                data_cluster_nb += (meta_idx + 1) / 2;
                atomic_blocks++;

                // Reset all counters, prepare for a new atomic block
                buf_offset = 0;
                read_since_last_flush = 0;
                meta_idx = 1;

                relative_offset = 0;

            }
        }

        footer.read += rb;

        if(footer.read % FIVE_GIB == 0) {
            if(footer.read > 0) {
                footer.ratio = ((double) footer.written / (double) footer.read);
            }
            fprintf(stderr, "Read %li, written %li, compression ratio %.5lf, data cluster number %li, atomic blocks %li\n",
                    footer.read, footer.written, footer.ratio, data_cluster_nb, atomic_blocks);
        }

    }

    if(rb < 0) {
        clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
        DIE("Unepxected error while reading from input\n");
    }

    // It may happen that the buffer is not empty. In such case we need to flush it
    // one last time
    if(buf_offset > 0) {
        fprintf(stderr, "Flushing last buffer to output\n");
        /* If we were not in a copy case, relative_offset contains the number of zeros
         * at the end of file. This number is redundant with the final footer read size.
         * Thanks to the footer.read number we will know, when inflating, how many zeros
         * we have to set in the end of file.
         * It will just be discarded anyway because meta_idx % 2 == 0 (see flush block)
         * So we may as well call flush block with relative offset-1
         */
        if(flush_block(buffer, buf_offset, &footer, dfp, meta_idx,
                       data_boundaries, relative_offset, random_size, random_buf)) {
            clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
            DIE("Flush block error\n");
        }

        atomic_blocks++;
        data_cluster_nb += meta_idx / 2 + (meta_idx % 2 != 0);
    }

    footer.atomic_blocks = atomic_blocks;

    fprintf(stderr, "Finished reading file !\n");

    if(footer.read > 0) {
        footer.ratio = ((double) footer.written/(double) footer.read);
    }

    //Push next block: -1 marks the start of the final footer
    buf_offset = -1L;
    written = fwrite(&buf_offset, sizeof(size_t), 1, dfp);
    if(written != 1) {
        clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
        DIE("Error declaring final footer\n");
    }
    footer.written += sizeof(size_t);

    footer.written += sizeof(sfs_footer_t);
    written = fwrite((void *) &footer, sizeof(sfs_footer_t), 1, dfp);
    if(written != 1) {
        clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
        DIE("Unable to write final footer correctly\n");
    }

    fprintf(stderr, "Read: %li, written %li, compression ratio %.5lf, number of atomic_blocks %li, "
            "data cluster number %li\n", footer.read, footer.written, footer.ratio,
            atomic_blocks, data_cluster_nb);

    clean_all(sfp, dfp, buffer, data_boundaries, random_buf);
    fprintf(stderr, "Sparse file stripper compression done!\n");

    exit(EXIT_SUCCESS);
}

