#include "print.h"
#include "init.h"
#include "debug.h"
#include "memory.h"
#include "thread.h"

static void k_thread_a(void* arg);
// 注意 main 函数要在第一位，链接的时候也需要确保 main.o 在第一位，否则要去修正 loader 中的 jump
int main(void)  {
    put_str("I am kernel\n");
    put_str("wei fei\n");
    init_all();

//    void* addr = get_kernel_pages(1);
//    put_str("\n get_kernel_page start vaddr is ");
//    put_int((uint32_t)addr);
//    put_str("\n");

    thread_start("k_thread_a", 31, k_thread_a, "K-thread-a-argA ");
    while(1);
}

static void k_thread_a(void* arg) {
    char* para = arg;
    while(1) {
        put_str(para);
    }
}