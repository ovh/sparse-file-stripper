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

#include <stdarg.h>
#include <stdio.h>

#include <sfs.h>


sfs_footer_t *extract_footer(FILE* sfp, int skip_repositionning) {
    size_t foot_size = sizeof(sfs_footer_t);
    size_t rb;
    sfs_footer_t *footp = malloc(sizeof(sfs_footer_t));
    if(footp == NULL) {
        fprintf(stderr, "Unable to allocate memory for footer\n");
        return NULL;
    }

    // best effort: relocate cursor whenever possible, no matter what the current position
    if(fseek(sfp, -foot_size, SEEK_END) != 0 && !skip_repositionning) {
        fprintf(stderr, "Unable to position correctly to %li bytes before end "
                        "of source file.\n", foot_size);
        free(footp);
        return NULL;
    }

    rb = fread(footp, 1, foot_size, sfp);
    if(rb != foot_size) {
        fprintf(stderr, "Unexpected number of bytes read (expected %li, actual %li)\n",
                foot_size, rb);
        free(footp);
        return NULL;
    }

    return footp;
}


void close_all_files(int fp_number, ...) {
    va_list valist;
    int i;
    FILE *fp = NULL;
    va_start(valist, fp_number);
    for(i = 0; i < fp_number; i++) {
        fp = va_arg(valist, FILE *);
        if(fp != NULL) {
            if(fclose(fp) != 0)
            fprintf(stderr, "Unable to close destination file correctly. Skipping\n");
        }
    }
    va_end(valist);
}


void free_all_mem(int voidp_number, ...) {
    va_list valist;
    int i;
    void *ap = NULL;
    va_start(valist, voidp_number);
    for(i = 0; i < voidp_number; i++) {
        ap = va_arg(valist, void *);
        if(ap != NULL) {
            free(ap);
        }
    }
    va_end(valist);
}
