#include <stdio.h>
#include "common.h"

int main(void)
{
    printf("boot: %d, work: %d\n", boot_code(), work_val());
    return 0;
}
