#ifndef _ZJUNIX_PID_H
#define _ZJUNIX_PID_H

#define PID_NUM 128     //最大进程数
#define PID_BYTES ((PID_NUM + 7) >> 3) //用于PID位图
#define IDLE_PID 0      //空进程
#define INIT_PID 1      //初始进程，为所有进程父进程

typedef unsigned int pid_t;
pid_t next_pid;         //下一个可分配PID
unsigned char pid_map[PID_BYTES];   //PID位图

void init_pid();
int pid_check(pid_t pid);
int pid_alloc(pid_t *ret);
int pid_free(pid_t pid);

#endif