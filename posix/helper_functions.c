#include "helper_functions.h"

#include <stdio.h>

void upgrade_fc_firmware(int(read)(void*,char*,int), void *readstate)
{
    fprintf(stderr, "Called\n");
    unsigned done = 0;
    char buffer[128];
    while (!done) {
        int num_read = read(readstate, buffer, sizeof(buffer));
        printf("read %u bytes\n", num_read);
        for (unsigned int i=0; i<num_read;i++) {
            fprintf(stderr, "%c", buffer[i]);
        }
        done = 1;
    }
}

