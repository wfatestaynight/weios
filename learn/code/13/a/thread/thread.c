#include "thread.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "debug.h"
#include "interrupt.h"
#include "print.h"
#include "memory.h"
#include "process.h"
#include "sync.h"

struct task_struct* main_thread;    // 主线程 PCB
struct task_struct* idle_thread;    // idle 线程
struct list thread_ready_list;	    // 就绪队列
struct list thread_all_list;	    // 所有任务队列
struct lock pid_lock;		    // 分配pid锁
static struct list_elem* thread_tag;// 用于保存队列中的线程结点

extern void switch_to(struct task_struct* cur, struct task_struct* next);

// 获取当前线程pcb指针
struct task_struct* running_thread() {
    uint32_t esp;
    asm ("mov %%esp, %0" : "=g" (esp));
    // 取 esp 整数部分即 pcb 起始地址
    return (struct task_struct*)(esp & 0xfffff000);
}

// 系统空闲时运行的线程
static void idle(void*) {
    while(1) {
        thread_block(TASK_BLOCKED);
        //执行 hlt 时必须要保证目前处在开中断的情况下
        asm volatile ("sti; hlt" : : : "memory");
    }
}

// 由 kernel_thread 去执行 function(func_arg)
static void kernel_thread(thread_func* function, void* func_arg) {
    // 执行function前要开中断, 避免后面的时钟中断被屏蔽, 而无法调度其它线程
    intr_enable();
    function(func_arg);
}

static pid_t allocate_pid(void) {
    static pid_t next_pid = 0;
    lock_acquire(&pid_lock);
    next_pid++;
    lock_release(&pid_lock);
    return next_pid;
}

// 初始化线程栈 thread_stack, 将待执行的函数和参数放到 thread_stack 中相应的位置
void thread_create(struct task_struct* pthread, thread_func function, void* func_arg) {
    // 先预留中断使用栈的空间, 可见 thread.h 中定义的结构
    pthread->self_kstack -= sizeof(struct intr_stack);
    // 再留出线程栈空间, 可见 thread.h 中定义
    pthread->self_kstack -= sizeof(struct thread_stack);
    struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
    kthread_stack->eip = kernel_thread;
    kthread_stack->function = function;
    kthread_stack->func_arg = func_arg;
    kthread_stack->ebp = kthread_stack->ebx = kthread_stack->esi = kthread_stack->edi = 0;
}
// 初始化线程基本信息
void init_thread(struct task_struct* pthread, char* name, int prio) {
    // 栈由高至低向下延伸，大小 4k, 低地址最下面是 task_struct, 如果魔数被修改说明栈溢出
    memset(pthread, 0, sizeof(*pthread));
    pthread->pid = allocate_pid();
    strcpy(pthread->name, name);

    if (pthread == main_thread) {
        pthread->status = TASK_RUNNING;
    } else {
        pthread->status = TASK_READY;
    }

    // self_kstack 是线程自己在内核态下使用的栈顶地址
    pthread->self_kstack = (uint32_t*)((uint32_t)pthread + PG_SIZE);
    pthread->priority = prio;
    pthread->ticks = prio;
    pthread->elapsed_ticks = 0;
    pthread->pgdir = NULL;
    pthread->stack_magic = 0x19870916;	  // 自定义的魔数
}

// 创建一优先级为 prio 的线程, 线程名为 name, 线程所执行的函数是 function(func_arg)
struct task_struct* thread_start(char* name, int prio, thread_func function, void* func_arg) {
    // pcb 都位于内核空间
    struct task_struct* thread = get_kernel_pages(1);
    init_thread(thread, name, prio);
    thread_create(thread, function, func_arg);

    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    list_append(&thread_ready_list, &thread->general_tag);

    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    list_append(&thread_all_list, &thread->all_list_tag);
    return thread;
}

static void make_main_thread(void) {
    // 因为main线程早已运行, 咱们在 loader.S 中进入内核时的 mov esp, 0xc009f000,
    // 就是为其预留了 pcb, 地址为 0xc009f000-0xc009e000,
    // 因此不需要通过 get_kernel_page 另分配一页
    main_thread = running_thread();
    init_thread(main_thread, "main", 31);

    // main函数是当前线程,当前线程不在 thread_ready_list 中,
    // 所以只将其加在 thread_all_list 中
    ASSERT(!elem_find(&thread_all_list, &main_thread->all_list_tag));
    list_append(&thread_all_list, &main_thread->all_list_tag);
}

void schedule() {
    ASSERT(intr_get_status() == INTR_OFF);

    struct task_struct* cur = running_thread();
    if (cur->status == TASK_RUNNING) {  // 若此线程只是cpu时间片到了,将其加入到就绪队列尾
        ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
        list_append(&thread_ready_list, &cur->general_tag);
        cur->ticks = cur->priority;     // 重新将当前线程的 ticks 再重置为其 priority;
        cur->status = TASK_READY;
    } else {
        // 若此线程需要某事件发生后才能继续上 cpu 运行
        // 不需要将其加入队列,因为当前线程不在就绪队列中
    }

    // 如果就绪队列中没有可运行的任务,就唤醒 idle
    if (list_empty(&thread_ready_list)) {
        thread_unblock(idle_thread);
    }

    ASSERT(!list_empty(&thread_ready_list));
    thread_tag = NULL;
    // 将 thread_ready_list 队列中的第一个就绪线程弹出, 准备将其调度上 cpu
    thread_tag = list_pop(&thread_ready_list);
    struct task_struct* next = elem2entry(struct task_struct, general_tag, thread_tag);
    next->status = TASK_RUNNING;

    // 激活任务页表等
    process_activate(next);

    switch_to(cur, next);
}

// 当前线程将自己阻塞, 标志其状态为 stat
void thread_block(enum task_status stat) {
    ASSERT(((stat == TASK_BLOCKED) || (stat == TASK_WAITING) || (stat == TASK_HANGING)));
    enum intr_status old_status = intr_disable();
    struct task_struct* cur_thread = running_thread();
    cur_thread->status = stat;
    schedule();		      // 将当前线程换下处理器
    // 待当前线程被解除阻塞后才继续运行下面的intr_set_status
    intr_set_status(old_status);
}

// 将线程 pthread 解除阻塞
void thread_unblock(struct task_struct* pthread) {
    enum intr_status old_status = intr_disable();
    ASSERT(((pthread->status == TASK_BLOCKED) || (pthread->status == TASK_WAITING) || (pthread->status == TASK_HANGING)));
    if (pthread->status != TASK_READY) {
        ASSERT(!elem_find(&thread_ready_list, &pthread->general_tag));
        if (elem_find(&thread_ready_list, &pthread->general_tag)) {
            PANIC("thread_unblock: blocked thread in ready_list\n");
        }
        list_push(&thread_ready_list, &pthread->general_tag);    // 放到队列的最前面,使其尽快得到调度
        pthread->status = TASK_READY;
    }
    intr_set_status(old_status);
}

// 主动让出 cpu, 换其它线程运行
void thread_yield(void) {
    struct task_struct* cur = running_thread();
    enum intr_status old_status = intr_disable();
    ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
    list_append(&thread_ready_list, &cur->general_tag);
    cur->status = TASK_READY;
    schedule();
    intr_set_status(old_status);
}

// 初始化线程环境
void thread_init(void) {
    put_str("thread_init start\n");
    list_init(&thread_ready_list);
    list_init(&thread_all_list);
    lock_init(&pid_lock);

    make_main_thread();
    // 创建 idle 线程
    idle_thread = thread_start("idle", 10, idle, NULL);
    put_str("thread_init done\n");
}