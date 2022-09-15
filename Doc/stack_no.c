#include <stdio.h>

static char inline stack_no(__U32_TYPE stack) { return ~((stack ^ 0x80000000) >> 23) ^ 0x80; }

int main(void){
    // __U32_TYPE stack_address = 0xbfffffff;
    __U32_TYPE stack_address = 0xc0000000;
    printf("Stack address: %p, Stack no: %d\n", stack_address, stack_no(stack_address));
    printf("Stack address: %p, Stack no: %d\n", 0xc0000000 -  STACK_SIZE, stack_no(0xc0000000 - STACK_SIZE));
}

