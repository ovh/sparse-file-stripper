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

//TODO: add some structure magic numbers to validate format,
// now there is absolutely no safeguard.

#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <sfs.h>

#define BUF_SIZE    256 * 1024 * 1024 // Buffer size to spare write ops

#define fmin(a,b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })


void print_usage () {
    fprintf(stderr, "sfsuz src_path dst_path\n");
}


// File cursor is assumed to be already at the start position to spare some fseek calls.
// Only useful for heavy zeroing anyway
void zero_from_current_and_move(FILE* dfp, size_t len,
                                dst_info_t* info) {
    int rc;
    int dstfd = fileno(dfp);
    int sector_size;
    char *zeros;
    size_t zeros_size, wb, start;

    assert(len > 0);

    // We assume provided offsets are sector aligned, otherwise fallocate
    // will fail. If you have some block devices with more than 4k sectors
    // this won't work
    start = ftell(dfp);
    if(start == -1L )
        DIE("Unable to get current position cursor for destination file");

    // Should we attempt a FALLOC_FL_ZERO_RANGE as fallback before filling explicitely zeroes out ?
    // Need further investigation, I do not see how the ZERO_RANGE could be supported (at least with a speed gain)
    // without the PUNCH_HOLE support as a first requirement (especially at the block device level)
    rc = 1;
    if(info->punch_support){
        rc = fallocate(dstfd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                       start, len);
        if(rc != 0) {
            // This is likely to spit out a lot of logs in case of failure. So we just log once and then update the
            fprintf(stderr, "Error: hole punching failed on range [%li, %li[. Probably not supported on destination. "
                    "Falling back on heavy zeroing. Perf will be highly degraded\n",
                    start, start+len);
            info->punch_support = 0;
        }
    }

    if(rc != 0) {
        if(ioctl(dstfd, BLKSSZGET, &sector_size) == -1)
            DIE("Unable to get sector size\n");

        // TODO: optimize here but this is not our use case...
        zeros_size = (size_t) fmin((double)BUF_SIZE, (double)len);
        zeros = malloc(zeros_size* sizeof(char));
        if(zeros == NULL)
            DIE("Unable to allocate memory for zeroing\n");
        memset(zeros, 0, zeros_size);
        while(len > 0) {
            // File cursor is assumed to be already at the start position to spare some fseek calls.
            zeros_size = (size_t) fmin((double)BUF_SIZE, (double)len);
            wb = fwrite(zeros, 1, zeros_size, dfp);
            if(wb != zeros_size)
                DIE("Heavy zeroing: unable to write to file correctly\n");
            len -= zeros_size;
        }
        free(zeros);
    }

    // Looks like fallocate does not move cursor, so let's do it
    if(fseek(dfp, len, SEEK_CUR) != 0) {
        fprintf(stderr, "Unable to move dst file cursor of %li bytes to the right\n",
                len);
        exit(EXIT_FAILURE);
    }

    return;
}


void free_all(FILE *sfp, FILE *dfp, char *atomic_block, size_t *data_boundaries, sfs_footer_t *footp, void *random_buf) {
    close_all_files(2, sfp, dfp);
    free_all_mem(4, (void *) atomic_block, (void *) data_boundaries, (void *) footp, (void *) random_buf);
}


//Destination is expected to be a seekable file (not a pipe)
int main(int argc, char *argv[]) {
    long i;
    int dfd;
    char *sfilename, *dfilename;
    FILE *sfp = NULL, *dfp = NULL;
    size_t *data_boundaries = NULL;
    char * atomic_block = NULL;
    size_t rb, wb;
    size_t data_seek, data_length, inflated = 0, atomic_read;
    size_t cursor, end_cursor;
    size_t max_atomic_block_size = 0, current_atomic_block_size;
    size_t meta_max_idx = 0, current_meta_max_idx, idx_upper_bound;
    size_t atomic_blocks = 0;
    sfs_footer_t *footp = NULL;
    size_t total_read = 0;
    size_t random_size_bytes;
    void *random_buf = NULL;
    dst_info_t dst_info;

    // We always assume punch support and eventually set it to 0 if some error
    // is encountered after first hole_punching attempt
    dst_info.punch_support = 1;

    fprintf(stderr, "Starting uncompression\n");

    //Positional arguments
    if(argc != 3) {
        print_usage();
        DIE("Missing mandatory param\n");
    }

    sfilename = argv[1];
    dfilename = argv[2];

    if(strcmp(sfilename, "-") == 0)
        sfp = freopen(NULL, "rb", stdin);
    else
        sfp = fopen(sfilename, "rb");

    if(sfp == NULL) {
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        DIE("Unable to open source file for reading\n");
    }

    // We cannot use fopen directly as we do not want to truncate file if it already exists)
    dfd = open(dfilename, O_WRONLY | O_CREAT, 0600);
    if(dfd == -1) {
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        DIE("Unable to open destination file for writing\n");
    }

    /* Now from the doc: fdopen
     * The meaning of these flags is exactly as specified in fopen(), except that modes
     * beginning with w do not cause the file to be truncated.
     */
    dfp = fdopen(dfd, "wb");
    if(dfp == NULL) {
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        DIE("Unable to open destination file for writting\n");
    }

    // First: determine whether the random buffer in every atomic block is activated or not
    rb = fread(&random_size_bytes, sizeof(size_t), 1, sfp);
    if(rb != 1) {
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        DIE("Unable to read random size from source \n");
    }
    total_read += sizeof(size_t);

    if(random_size_bytes > 0) {
        fprintf(
            stderr,
            "Random bufferes activated. Allocating garbage buffer with %li bytes\n",
            random_size_bytes
        );
        random_buf = malloc(random_size_bytes);
        if(random_buf == NULL) {
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            DIE("Unable to allocate random buffer\n");
        }
    }

    // Read atomic blocks one by one
    while((rb = fread(&current_atomic_block_size, sizeof(size_t), 1, sfp)) > 0) {

        if(rb != 1) {
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            DIE("Unable to read atomic block size from source \n");
        }

        total_read += sizeof(size_t);

        if(current_atomic_block_size == -1L) {
            fprintf(stderr, "All atomic blocks read. Footer remaining\n");
            break;
        }
        atomic_blocks++;

        // TODO: use a more robust data integrity check here, like a checksum
        if((current_atomic_block_size <= 0) || (current_atomic_block_size > 4294967296)) {
            fprintf(stderr, "Unexpected atomic block size %li, should be > 0 and < 4294967296\n",
                    current_atomic_block_size);
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            exit(EXIT_FAILURE);
        }

        // Discard random buffer if any
        if(random_size_bytes > 0) {
            rb = fread(random_buf, random_size_bytes, 1, sfp);
            if(rb != 1) {
                free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
                DIE("Unable to discard random buffer from block \n");
            }
            total_read += random_size_bytes;
        }

        // Inflate atomic block
        if(max_atomic_block_size < current_atomic_block_size) {
            fprintf(stderr, "Extending atomic block buffer by %li bytes\n",
                    current_atomic_block_size - max_atomic_block_size);
            atomic_block = realloc(atomic_block, current_atomic_block_size);
            if(atomic_block == NULL) {
                fprintf(stderr, "Unable to allocate %li bytes of memory for buffer. "
                        "Block size was too big when compressing for this server to "
                        "be able to inflate data\n", current_atomic_block_size);
                free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
                exit(EXIT_FAILURE);
            }
            max_atomic_block_size = current_atomic_block_size;
        }

        rb = fread(atomic_block, 1, current_atomic_block_size, sfp);
        if(rb != current_atomic_block_size) {
            fprintf(stderr, "Read bytes: %li. Differs from expected atomic block size: "
                    "%li bytes.\n", rb, current_atomic_block_size);
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            exit(EXIT_FAILURE);
        }
        total_read += current_atomic_block_size;

        // Now load offsets
        rb = fread(&current_meta_max_idx, sizeof(size_t), 1, sfp);
        if(rb != 1) {
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            DIE("Unable to extract offsets array length\n");
        }

        total_read += sizeof(size_t);

        /* TODO: improve data integrity checks.
         */
        idx_upper_bound = (current_atomic_block_size / BLK_SIZE + 1) * 2;
        if(
            (current_meta_max_idx <= 0) ||
            (current_meta_max_idx % 2 != 0) ||
            (current_meta_max_idx > idx_upper_bound)
        ) {
            fprintf(stderr,
                    "Unconsistent data: current_meta_max_index (%li) does not meet "
                    "expected requirements (positive and even integer lower than %li)\n",
                    current_meta_max_idx, idx_upper_bound);
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            exit(EXIT_FAILURE);
        }

        if(current_meta_max_idx > meta_max_idx) {
            fprintf(stderr, "Extending offsets array by %li bytes\n",
                    (current_meta_max_idx - meta_max_idx) * sizeof(size_t));
            data_boundaries = realloc(data_boundaries, current_meta_max_idx * sizeof(size_t));
            if(data_boundaries == NULL) {
                fprintf(stderr, "Unable to allocate %li bytes of memory for data boundaries. "
                        "Block size was too big when compressing for this server to "
                        "be able to inflate data\n", current_meta_max_idx);
                free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
                exit(EXIT_FAILURE);
            }
            meta_max_idx = current_meta_max_idx;
        }

        rb = fread(data_boundaries, sizeof(size_t), current_meta_max_idx, sfp);
        if(rb != current_meta_max_idx) {
            fprintf(stderr, "Read: %li longs. Differs from expected: %li longs\n",
                    rb, current_meta_max_idx);
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            exit(EXIT_FAILURE);
        }
        total_read += sizeof(size_t) * current_meta_max_idx;

        //TODO: once again, improve data integrity checks here
        if((data_boundaries == NULL) || (data_boundaries[0] != 0)) {
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            DIE("Unconsistent data: unexpected offset array\n");
        }

        atomic_read = 0;
        //By convention we start by assuming sparse mode is off
        for(i=0; i<current_meta_max_idx; i+=2) {
            //Data offsets in bytes
            data_seek = data_boundaries[i];
            data_length = data_boundaries[i+1];
            inflated += data_seek + data_length;

            if(atomic_read + data_length > current_atomic_block_size) {
                fprintf(stderr, "Unconsistent data: %li > %li\n", atomic_read + data_length, current_atomic_block_size);
                free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
                DIE("Unconsistent data: offset array item falls out of bounds\n");
            }

            if(data_length == 0 || data_seek == 0) {
                // This can only happen at the start of the file
                if(i > 0) {
                    fprintf(stderr, "A zero length sparse or data region should not be possible "
                                    "apart at the file beginning. Index %li, sparse len %li, "
                                    "data len %li.\n",
                            i, data_seek, data_length);
                    free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
                    DIE("Unconsistent data: invalid metadata\n");
                }
                if(data_length == 0)
                    continue;
            }

            //Either success or die anyway so no need to check anything here
            if(data_seek > 0)
                zero_from_current_and_move(dfp, data_seek, &dst_info);

            wb = fwrite(atomic_block+atomic_read, 1, data_length, dfp);
            atomic_read += data_length;
            if(wb != data_length) {
                fprintf(stderr, "Unexpected number of bytes written to destination. "
                                "Expected %li, actual %li\n", data_length, wb);
                free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
                DIE("Unable to write data correctly on destination!\n");
            }
        } // Block data read

        if(atomic_read != current_atomic_block_size) {
            fprintf(stderr,
                    "Unconsistent data: atomic read (%li) differs from expected (%li)\n",
                    atomic_read, current_atomic_block_size);
            free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
            exit(EXIT_FAILURE);
        }
    }

    fprintf(stderr, "All non-zero data written. Extracting final footer\n");

    footp = extract_footer(sfp, 1);
    if(footp == NULL) {
        fprintf(stderr,
                "Unable to extract footer correctly\n");
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        exit(EXIT_FAILURE);
    }
    total_read += sizeof(sfs_footer_t);

    fprintf(stderr, "Check footer info consistency\n");

    //fprintf(stderr, "footp written %li\n", footp->written);
    fprintf(stderr, "total read %li\n", total_read);
    fprintf(stderr, "Inflated %li\n", inflated);

    if(footp->written != total_read) {
        fprintf(stderr, "Unconsistent data: footer info (%li) differs from what was really read (%li)\n",
                footp->written, total_read);
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        exit(EXIT_FAILURE);
    }

    if(footp->atomic_blocks != atomic_blocks)
    {
        fprintf(stderr, "Unconsistent data: footer atomic blocks (%li) differs from reality (%li)\n",
                footp->atomic_blocks, atomic_blocks);
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        exit(EXIT_FAILURE);
    }

    // If footp->read < inflated then it means that we have some unconsistency between the footer and the offsets array
    if(footp->read < inflated) {
        /* Note(rg): if we created a header per atomic block instead of a final footer, this would deteriorate
         * the compression ratio and the inflate overall performances, but this would allow to control the inflated
         * size more quickly, not only at the very end (resource exhaustion protection)
         */
        fprintf(stderr,
                "Unconsistent data: inflated volume (%li) bigger than what is reported in footer (%li)\n",
                inflated, footp->read
        );
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        exit(EXIT_FAILURE);
    }

    data_seek = footp->read - inflated;
    cursor = ftell(dfp);

    // This trick is to make sure the final inflated file is at least as big as the source one
    // in the specific case when the source files ends with zeros
    if(data_seek > 0) {
        fprintf(stderr, "Remaining number of zeros to write: %li bytes\n", data_seek);
        rb = (data_seek - 1) / BLK_SIZE * BLK_SIZE;
        if(rb > 0) {
            fprintf(stderr, "Falloc %li bytes\n", rb);
            zero_from_current_and_move(dfp, rb, &dst_info);
        }

        rb = (data_seek - 1) % BLK_SIZE + 1;
        if(rb > 0) {
            fprintf(stderr, "Remaining zeros: %li bytes\n", rb);

            if(rb > max_atomic_block_size){
                // This should not happen as the atomic block size is expected to be >= BLK_SIZE
                atomic_block = realloc(atomic_block, rb);
                if(atomic_block == NULL) {
                    fprintf(stderr, "Unable to allocate memory\n");
                    free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
                    DIE("Memory error");
                }
            }
            memset(atomic_block, 0, rb);
            wb = fwrite(atomic_block, 1, rb, dfp);
            if(wb != rb) {
                fprintf(stderr, "Unexpected number of bytes written (%li != %li)\n",
                    wb, rb);
                free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
                DIE("Unable to write end of file\n");
            }
        }
    }

    fprintf(stderr, "All data written. Zeroing any left space in file if any\n");

    if(fseek(dfp, 0, SEEK_END) != 0 ) {
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        DIE("Unable to position self at the end of dst\n");
    }

    end_cursor = ftell(dfp);
    if(end_cursor == EOF) {
        free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);
        DIE("Unable to get current position on destination\n");
    }

    if(end_cursor < cursor + data_seek) {
        fprintf(stderr, "WARNING: dst file was smaller than source, "
                "%li zeros could not be written. Ignoring.\n",
                data_seek - end_cursor + cursor);
    }

    free_all(sfp, dfp, atomic_block, data_boundaries, footp, random_buf);

    fprintf(stderr, "All done\n");

    exit(EXIT_SUCCESS);
}
