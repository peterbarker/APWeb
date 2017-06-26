#ifndef POSIX_HELPER_FUNCTIONS_H
#define POSIX_HELPER_FUNCTIONS_H

#include <stdint.h>

void upgrade_fc_firmware(int(read)(void*,char*,int), void *readstate, unsigned long total_length);

#endif
