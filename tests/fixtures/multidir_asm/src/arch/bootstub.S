#include "common.h"

.globl boot_code
#if defined(__ELF__) && defined(__x86_64__)
.type boot_code, @function
#endif
boot_code:
#if defined(__x86_64__)
    movl $MAGIC_NUM, %eax
    ret
#elif defined(__aarch64__)
    mov w0, #MAGIC_NUM
    ret
#else
    movl $MAGIC_NUM, %eax
    ret
#endif
