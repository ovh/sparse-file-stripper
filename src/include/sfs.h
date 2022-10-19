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

#include <stdio.h>
#include <stdlib.h>

#define BLK_SIZE    4096 // Minimum number of contiguous zeros to switch on sparse mode
#define DIE(msg)    { fprintf(stderr, msg); exit(EXIT_FAILURE); }

typedef struct sfs_footer {
    size_t read;
    size_t written;
    double ratio;
    size_t atomic_blocks;
} sfs_footer_t; // The 24 last bytes of the file will contain this struct.


typedef struct dst_info_t {
    u_int8_t punch_support;
} dst_info_t;

sfs_footer_t *extract_footer(FILE* sfp, int skip_repositionning);

void close_all_files(int fp_number, ...);

void free_all_mem(int voidp_number, ...);
