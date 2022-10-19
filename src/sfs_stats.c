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


#include <sfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


int main(int argc, char *argv[])
{
    char *sfilename;
    FILE *sfp;
    sfs_footer_t *footerp;

    if(argc != 2)
        DIE("Missing argument, usage: sfs_stats filename\n");

    sfilename = argv[1];

    sfp = fopen(sfilename, "rb");
    if(sfp == NULL)
        DIE("Unable to open source file for reading\n");

    footerp = extract_footer(sfp, 0);
    if(footerp == NULL)
        DIE("Unable to extract footer from source\n");

    fprintf(stdout, "Sparse file stripper stats: read=%li, written=%li, "
            "ratio=%.5lf, atomic_blocks=%li\n", footerp->read, footerp->written,
            footerp->ratio, footerp->atomic_blocks);
    free(footerp);
    fclose(sfp);

    exit(EXIT_SUCCESS);
}
