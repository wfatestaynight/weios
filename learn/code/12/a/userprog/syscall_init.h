#ifndef __USERPROG_SYSCALL_INIT_H__
#define __USERPROG_SYSCALL_INIT_H__
#include "stdint.h"
void syscall_init(void);
uint32_t sys_getpid(void);
uint32_t sys_write(char* str);
#endif  //__USERPROG_SYSCALL_INIT_H__