#include "print.h"
#include "init.h"
#include "debug.h"
#include "memory.h"

int main(void)  {
    put_str("I am kernel\n");
    put_str("wei fei\n");
    init_all();

    void* addr = get_kernel_pages(3);
    put_str("\n get_kernel_page start vaddr is ");
    put_int((uint32_t)addr);
    put_str("\n");

    void* addr2 = get_kernel_pages(2);
    put_str("\n get_kernel_page start vaddr2 is ");
    put_int((uint32_t)addr2);
    put_str("\n");
    while(1);
}