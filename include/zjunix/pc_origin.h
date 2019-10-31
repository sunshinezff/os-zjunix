#ifndef _ZJUNIX_PC_H
#define _ZJUNIX_PC_H

typedef struct {
    unsigned int epc; // 进程重新开始执行的指令地址
    unsigned int at;
    unsigned int v0, v1;
    unsigned int a0, a1, a2, a3;
    unsigned int t0, t1, t2, t3, t4, t5, t6, t7;
    unsigned int s0, s1, s2, s3, s4, s5, s6, s7;
    unsigned int t8, t9;
    unsigned int hi, lo;
    unsigned int gp; // 全局指针
    unsigned int sp; // 堆栈指针
    unsigned int fp; // 帧指针
    unsigned int ra; // 返回地址
} context; 
// 寄存器信息
// extensively used when loading the entry point and args of function

typedef struct {
    context context;
    int ASID; // Adress Space ID
    unsigned int counter;
    char name[32]; // 进程名
    unsigned long start_time; // 创建时间
} task_struct; // 进程控制块

// 注意：union
typedef union {
    task_struct task;
    unsigned char kernel_stack[4096]; // 进程内核栈
} task_union; // 统一存储控制块和内核栈

#define PROC_DEFAULT_TIMESLOTS 6

void init_pc();
void pc_schedule(unsigned int status, unsigned int cause, context* pt_context);
int pc_peek();
void pc_create(int asid, void (*func)(), unsigned int init_sp, unsigned int init_gp, char* name);
void pc_kill_syscall(unsigned int status, unsigned int cause, context* pt_context);
int pc_kill(int proc);
task_struct* get_curr_pcb();
int print_proc();

#endif  // !_ZJUNIX_PC_H