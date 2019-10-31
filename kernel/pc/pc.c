#include "pc.h"

#include <driver/vga.h>
#include <intr.h>
#include <zjunix/syscall.h>
#include <zjunix/utils.h>

//所有进程链表
struct list_head tasks;
//等待进程链表
struct list_head wait;
//终结进程链表
struct list_head terminal;
//调度链表，最后为空进程
struct list_head sched[PRORITY_NUM + 1];
//各优先级时间片
unsigned int sched_time[PRORITY_NUM];
//当前运行进程指针
task_struct * current_task = 0;

// save context when doing context switch in interrupt
static void copy_context(context* src, context* dest) {
    dest->epc = src->epc;
    dest->at = src->at;
    dest->v0 = src->v0;
    dest->v1 = src->v1;
    dest->a0 = src->a0;
    dest->a1 = src->a1;
    dest->a2 = src->a2;
    dest->a3 = src->a3;
    dest->t0 = src->t0;
    dest->t1 = src->t1;
    dest->t2 = src->t2;
    dest->t3 = src->t3;
    dest->t4 = src->t4;
    dest->t5 = src->t5;
    dest->t6 = src->t6;
    dest->t7 = src->t7;
    dest->s0 = src->s0;
    dest->s1 = src->s1;
    dest->s2 = src->s2;
    dest->s3 = src->s3;
    dest->s4 = src->s4;
    dest->s5 = src->s5;
    dest->s6 = src->s6;
    dest->s7 = src->s7;
    dest->t8 = src->t8;
    dest->t9 = src->t9;
    dest->hi = src->hi;
    dest->lo = src->lo;
    dest->gp = src->gp;
    dest->sp = src->sp;
    dest->fp = src->fp;
    dest->ra = src->ra;
}

//初始化所有进程链表
//在init_pc()中调用
void init_pc_list(){
    INIT_LIST_HEAD(&wait);
    INIT_LIST_HEAD(&terminal);
    INIT_LIST_HEAD(&tasks);
    for(int i = 0; i < PRORITY_NUM; i++){
        INIT_LIST_HEAD(&sched[i]);
        sched_time[i] = MIN_TIMESLICE * (i + 1);
    }
    //空进程链表
    INIT_LIST_HEAD(&sched[PRORITY_NUM]);
}

//初始化优先级位图
void init_pro_map(){
    for(int i = 0; i < PRORITY_BYTES; i++){
        pro_map[i] = 0;
    }
}

//将进程加入所有进程链表
void add_tasks(task_struct * task){
    list_add_tail(&(task->list), &tasks);
}

//将进程加入调度链表
//空进程加入末尾，其他进程按照动态优先级加入
void add_sched(task_struct * task){
    int index = task->dynamic_prority;
    if(index == -1){
        list_add_tail(&(task->sched), &sched[PRORITY_NUM]);
    }
    else if(index >= PRORITY_NUM || index < -1){
        kernel_printf("Add_sched: index out of range!\n");
    }
    else{
        list_add_tail(&(task->sched), &sched[index]);
    }
}

//将进程加入等待链表
void add_wait(task_struct * task){
    list_add_tail(&(task->sched), &wait);
}

//将进程加入优先级位图
void add_pro_map(task_struct * task){
    int pro = task->dynamic_prority;
    if(pro == -1){
        return;
    }
    else{
        int map_index = task->dynamic_prority >> 3;
        int byte_index = task->dynamic_prority & 7;
        pro_map[map_index] |= (1 << byte_index);
        return;
    }
}

//初始化进程管理，创建空进程
//在init_kernel()中调用
void init_pc(){
    //空进程
    task_struct *idle;

    //初始化进程管理有关的全局链表
    init_pc_list();

    //初始化优先级位图
    init_pro_map();

    //创建空进程
    //空进程的task_struct结构位于内核代码部分(0-16MB)的最后一页
    idle = (task_struct * )(160000 - KERNEL_STACK_SIZE);
    
    //空进程结构初始化
    idle->pid = IDLE_PID;
    idle->ASID = idle->pid;
    idle->state = TASK_UNINIT;
    idle->ppid = idle->pid;
    kernel_strcpy(idle->name, "idle");
    idle->static_prority = -1;
    idle->dynamic_prority = idle->static_prority;
    idle->counter = MAX_TIMESLICE;
    kernel_strcpy(idle->start_time, "00:00:00");
    idle->sleep_avg = 0;
    idle->is_changed = 0;
    
    //当前寄存器的内容即为空进程的寄存器内容无需赋值

    INIT_LIST_HEAD(&(idle->sched));
    INIT_LIST_HEAD(&(idle->list));
    idle->mm = 0;
    //idle->files = 0;
    add_tasks(idle);
    add_sched(idle);

    idle->state = TASK_READY;
    current_task = idle;

    //注册进程调度函数，时钟中断触发
    register_interrupt_handler(7, pc_schedule);
    //设置cp0中的compare和count寄存器
    //当compare == count时，产生时钟中断（7号）
    asm volatile(
        "li $v0, 10000000\n\t"
        "mtc0 $v0, $11\n\t"
        "mtc0 $zero, $9");

}

//创建新的进程
//task_name: 进程名
//entry: 进程的入口函数
//argv: 进程的参数信息
//ret_pid：用于返回新创建进程的PID
//创建成功返回0，否则返回1
int task_create(char * task_name, long static_prority, void (*entry)(unsigned int argc, void * argv),
                unsigned int argc, void * argv, pid_t * ret_pid, int is_user){
    //检查静态优先级
    if(static_prority >= PRORITY_NUM || static_prority < 0){
        kernel_printf("Task_create: static_prority out of range!\n");
        return 1;
    }
    
    //分配PID
    pid_t new_pid;
    if(pid_alloc(&new_pid)){
        kernel_printf("Task_create: pid allocated failed!\n");
        return 1;
    }

    //创建task_union结构
    task_union * new_union;
    new_union = (union task_union*)kmalloc(sizeof(task_union));
    if(new_union == 0){
        kernel_printf("Task_create: task_union allocated failed\n");
        if(pid_free(new_pid)){
            kernel_printf("Task_create: pid freed failed!\n");
        }
        return 1;
    }

    //初始化task_struct结构信息
    new_union->task.pid = new_pid;
    new_union->task.ASID = new_union->task.pid;
    new_union->task.state = TASK_UNINIT;
    new_union->task.ppid = current_task->pid;
    kernel_strcpy(new_union->task.name, task_name);
    
    // #ifdef PC_DEBUG
    //     kernel_printf("task_name = %s\n", task_name);
    // #endif

    //init进程
    if(new_union->task.pid == INIT_PID){
        new_union->task.static_prority = -1;
        new_union->task.dynamic_prority = new_union->task.static_prority;
        new_union->task.counter = MAX_TIMESLICE;
    }
    //其他进程
    else{
        new_union->task.static_prority = static_prority;
        new_union->task.dynamic_prority = new_union->task.static_prority;
        new_union->task.counter = sched_time[new_union->task.static_prority];
    }
    char temp_time[START_TIME_LEN];
    get_time(temp_time, START_TIME_LEN);
    kernel_strcpy(new_union->task.start_time, temp_time);
    new_union->task.sleep_avg = 0;
    new_union->task.is_changed = 0;

    //寄存器初始化
    kernel_memset(&(new_union->task.context), 0, sizeof(context));
    //新进程入口地址
    new_union->task.context.epc = (unsigned int)entry;
    //新进程内核栈指针
    new_union->task.context.sp = (unsigned int)new_union + KERNEL_STACK_SIZE;
    //设置全局指针
    unsigned int init_gp;
    asm volatile("la %0, _gp\n\t" : "=r"(init_gp));
    new_union->task.context.gp = init_gp;
    //设置新进程参数
    new_union->task.context.a0 = argc;
    new_union->task.context.a1 = (unsigned int)argv;

    INIT_LIST_HEAD(&(new_union->task.sched));
    INIT_LIST_HEAD(&(new_union->task.list));

    //用户进程空间结构
    //if(is_user){
        //new_union->task.mm = mm_create();
    //}
    //else{
        new_union->task.mm = 0;
    //}
    //打开文件链表
    //new_union->task.files = 0;

    //返回进程pid
    if(ret_pid != 0){
        *ret_pid = new_union->task.pid;
    }

    //加入进程链表
    add_tasks(&(new_union->task));
    add_sched(&(new_union->task));
    add_pro_map(&(new_union->task));
    new_union->task.state = TASK_READY;
    return 0;
}

//从终结链表中移除进程
//在清理结束链表clear_terminal()中调用
void remove_terminal(task_struct * task){
    list_del(&(task->sched));
    INIT_LIST_HEAD(&(task->sched));
}

//从所有进程链表中移除进程
//在清理结束链表clear_terminal()中调用
void remove_tasks(task_struct * task){
    list_del(&(task->list));
    INIT_LIST_HEAD(&(task->list));
}

//从优先级调度链表中移除进程
void remove_sched(task_struct * task){
    list_del(&(task->sched));
    INIT_LIST_HEAD(&(task->sched));
}

//清理终结链表
//在pc_schedule()中调用
void clear_terminal(){
    task_struct * task;

    //删除terminal链表第一个节点直到terminal为空
    while(terminal.next != &terminal){
        task = container_of(terminal.next, task_struct, sched);
        #ifdef PC_DEBUG
            int temp_pid = task->pid;
        #endif
        if(task->state != TASK_TERMINAL){
            kernel_printf("Clear_terminal: one task in terminal list with state != TASK_TERMINAL! --%S\n", task->name);
            while(1);
        }

        remove_terminal(task);
        remove_tasks(task);

        #ifdef PC_DEBUG
            kernel_printf("Clear_terminal: task with pid = %d is cleared\n", temp_pid);
        #endif
    }
    return;
}

//更新平均睡眠时间
//在updata_dynamic_prority()中调用
void update_sleep_avg(){
    int sleep_time = (current_task->dynamic_prority + 1) * MIN_TIMESLICE;
    //遍历优先级调度链表，跟新平均睡眠时间
    struct list_head *pos;
    task_struct *next;
    for(int i = 0; i < PRORITY_NUM; i++){
        //和当前进程不在同一优先级
        if(i != current_task->dynamic_prority){
            list_for_each(pos, &sched[i]){
                next = container_of(pos, task_struct, sched);
                next->sleep_avg += sleep_time;
            }
        }
        //和当前进程属于同一优先级
        else{
            list_for_each(pos, &sched[i]){
                next = container_of(pos, task_struct, sched);
                //不是当前进程则增加睡眠时间
                if(next != current_task){
                    next->sleep_avg += sleep_time;
                }
                //是当前进程则减少睡眠时间
                else{
                    next->sleep_avg -= sleep_time;
                }
            }
        }
    }
}

//计算链表节点个数
int count_list(struct list_head * head){
    int count = 0;
    struct list_head * pos;
    //遍历链表
    list_for_each(pos, head){
        count++;
    }
    return count;
}

//根据平均睡眠时间更新动态优先级
//当前执行进程睡眠时间减少，其他进程睡眠时间增加，init和idle不变
void update_dynamic_prority(){

    //更新平均睡眠时间
    update_sleep_avg();

    struct list_head * pos;
    task_struct * next;
    
    //根据平均睡眠时间修改动态优先级
    for(int i = 0; i < PRORITY_NUM; i++){
        int num = count_list(&sched[i]);
        struct list_head * temp = sched[i].next;
        while(num){
            num--;
            pos = temp;
            temp = temp->next;
            next = container_of(pos, task_struct, sched);
            //当前进程不变
            if(next != current_task){
                remove_sched(next);
                //已经过变化则不再变化
                if(!next->is_changed){
                    // #ifdef PC_DEBUG
                    //     kernel_printf("Update_d_prority: pre_prority: %d with pid = %d\n", next->dynamic_prority, next->pid);
                    // #endif
                    int temp = next->dynamic_prority;
                    next->dynamic_prority += next->sleep_avg / (PRORITY_NUM * MIN_TIMESLICE);
                    if(next->dynamic_prority >= PRORITY_NUM){
                        next->dynamic_prority = PRORITY_NUM - 1;
                    }
                    else if(next->dynamic_prority < 0){
                        next->dynamic_prority = 0;
                        //next->sleep_avg = 0;
                    }
                    next->counter = sched_time[next->dynamic_prority];
                    next->is_changed = 1;

                    // #ifdef PC_DEBUG
                    //     kernel_printf("Update_d_prority: new_prority: %d with pid = %d\n", next->dynamic_prority, next->pid);
                    // #endif
                    #ifdef PC_DEBUG
                        int new = next->dynamic_prority;
                        if(temp != new){
                            kernel_printf("task pid = %d: pre_prority = %d -> new_prority = %d\n", next->pid, temp, new);
                        }
                    #endif
                }
                add_sched(next);
            }
        }
    }
}

//更新优先级位图
void update_pro_map(){

    //初始化位图
    init_pro_map();

    task_struct * next;
    for(int i = 0; i < PRORITY_NUM; i++){
        if(sched[i].next != &sched[i]){
            next = container_of(sched[i].next, task_struct, sched);
            add_pro_map(next);
        }
    }
}

//更新是否变化
void update_is_changed(){
    struct list_head * pos;
    task_struct * next;
    
    //遍历所有进程链表
    list_for_each(pos, &tasks){
        next = container_of(pos, task_struct, list);
        next->is_changed = 0;
    }
}

//在优先级位图中寻找最高优先级进程
task_struct * find_in_pro_map(){
    task_struct * next;
    int i;
    for(i = PRORITY_NUM - 1; i >= 0; i--){
        int map_index = i >> 3;
        int byte_index = i & 7;
        if(pro_map[map_index] & (1 << byte_index)){
            break;
        }
    }
    //优先级链表无进程返回空进程
    if(i < 0){
        next = container_of(sched[PRORITY_NUM].next, task_struct, sched);
    }
    //优先级链表有进程
    else{
         next = container_of(sched[i].next, task_struct, sched);
    }
    return next;
}

//选取下一个要运行的进程，返回其task_struct结构
//采用动态优先级调度算法，总是选取具有最高优先级的进程，同一优先级中选取链表中的第一个进程
task_struct * find_next_task(){
    task_struct * next;
    int is_back = 0;

    //init进程
    if(current_task->sched.next == &sched[PRORITY_NUM]){
        is_back = 1;
    }
    //idle进程
    else if(current_task->dynamic_prority == -1 && current_task->sched.next != &sched[PRORITY_NUM]){
        next = container_of(current_task->sched.next, task_struct, sched);
    }
    //优先级进程
    else{
        remove_sched(current_task);
        //已经过变化则不再变化
        if(!current_task->is_changed){
            int temp = current_task->dynamic_prority;
            // #ifdef PC_DEBUG
            //     kernel_printf("Update_d_prority: pre_prority: %d with pid = %d\n", current_task->dynamic_prority, current_task->pid);
            // #endif
            current_task->dynamic_prority += current_task->sleep_avg / (PRORITY_NUM * MIN_TIMESLICE);
            if(current_task->dynamic_prority >= PRORITY_NUM){
                current_task->dynamic_prority = PRORITY_NUM - 1;
            }
            else if(current_task->dynamic_prority < 0){
                current_task->dynamic_prority = 0;
                //current_task->sleep_avg = 0;
            }
            current_task->counter = sched_time[current_task->dynamic_prority];
            current_task->is_changed = 1;
            // #ifdef PC_DEBUG
            //     kernel_printf("Update_d_prority: new_prority: %d with pid = %d\n", current_task->dynamic_prority, current_task->pid);
            // #endif

            #ifdef PC_DEBUG
                int new = current_task->dynamic_prority;
                if(temp != new){
                    kernel_printf("task pid = %d: pre_prority = %d -> new_prority = %d\n", current_task->pid, temp, new);
                }
            #endif
        }
        add_sched(current_task);

        //更新优先级位图
        update_pro_map();
        //更新是否改变优先级
        update_is_changed();

        next = container_of(sched[PRORITY_NUM].next, task_struct, sched);
    }
    if(is_back == 1){
        next = find_in_pro_map();
    }
    return next;
}

//进程调度函数，由时钟中断触发
//参数pt_context指向当前进程的上下文信息
void pc_schedule(unsigned int status, unsigned int cause, context * pt_context){
    // #ifdef PC_DEBUG
    //     kernel_printf("PC_schedule: current_pid = %d\n", current_task->pid);
    // #endif

    task_struct * next;
    //若非idle、init进程则更改时间片数量
    if(current_task->dynamic_prority != -1){
        current_task->counter--;

        // #ifdef PC_DEBUG
        //     kernel_printf("PC_schedule: current_task counter: %d\n", current_task->counter);
        // #endif

        //判断当前进程时间片是否已用完,若没用完继续运行，若用完执行调度算法
        if(current_task->counter != 0){
            goto end;
        }
        else{
            //清理终结链表
            clear_terminal();

            //更新动态优先级
            update_dynamic_prority();

            //调用调度算法，选取下一个要运行的进程
            next = find_next_task();
        }
    }
    else{
        //清理终结链表
        clear_terminal();
        //调用调度算法，选取下一个要运行的进程
        next = find_next_task();
    }

    //如果选取的进程不是当前进程
    if(next != current_task){
        // if(next->mm != 0){
        //     activate_mm(next);
        // }

        //保存当前进程上下文
        copy_context(pt_context, &(current_task->context));
        current_task->state = TASK_READY;
        current_task = next;
        //加载下一进程上下文
        copy_context(&(current_task->context), pt_context);
        current_task->state = TASK_RUNNING;
        goto end;
    }    
    else{
        kernel_printf("PC_schedule: next == current_task\n");
        while(1);
        goto end;
    }

end:
    // #ifdef PC_DEBUG
    //     kernel_printf("PC_shcedule: next_pid = %d\n", current_task->pid);
    // #endif
    //将cp0中到count寄存器复位为0，结束时钟中断
    asm volatile("mtc0 $zero, $9\n\t");
}

//打印进程结构信息
void print_task_struct(task_struct * task){
    kernel_printf("name: %s \t pid: %d \t ppid: %d \t ", task->name, task->pid, task->ppid);
    kernel_printf("s_prority: %d \t d_prority: %d \t ", task->static_prority, task->dynamic_prority);
    switch(task->state){
        case 0: kernel_printf("state: UNINIT\n");break;
        case 1: kernel_printf("state: READY\n");break;
        case 2: kernel_printf("state: RUNNING\n");break;
        case 3: kernel_printf("state: WAIT\n");break;
        case 4: kernel_printf("state: TERMINAL\n");break;
        default:kernel_printf("state: UNDEFINE\n");break;
    }
}

//打印进程链表信息
int print_proc(){
    struct list_head * pos;
    task_struct * next;

    kernel_printf("ps results:\n");
    list_for_each(pos, &tasks){
        next = container_of(pos, task_struct, list);
        print_task_struct(next);
    }
    return 0;
}

//根据PID在所有进程链表中查找进程结构
task_struct * find_in_tasks(pid_t pid){
    struct list_head * pos;
    task_struct * next;
    task_struct * ret = 0;

    //遍历所有进程链表
    list_for_each(pos, &tasks){
        next = container_of(pos, task_struct, list);
        if(next->pid == pid){
            ret = next;
            break;
        }
    }
    return ret;
}

//根据PPID在等待队列中查找进程结构
task_struct * find_in_wait(pid_t ppid){
    struct list_head * pos;
    task_struct * next;
    task_struct * ret = 0;

    //遍历等待队列
    list_for_each(pos, &wait){
        next = container_of(pos, task_struct, sched);
        if(next->pid == ppid){
            ret = next;
            break;
        }
    }
    return ret;   
}

//将进程加入终结链表
void add_terminal(task_struct * task){
    list_add_tail(&(task->sched), &terminal);
}

//关闭进程文件链表
// void task_files_delete(task_struct * task){
//     fat32_close(task->files);
//     kfree(&(task->files));
// }

//根据输入进程号杀死进程
//将杀死的进程从优先级链表/等待链表中移除并加入终结链表
//返回0表示执行成功，否则执行失败
int pc_kill(pid_t pid){
    //idle进程不能被杀死
    if(pid == IDLE_PID){
        kernel_printf("PC_kill: idle process can not be killed!\n");
        return 1;
    }

    //init进程不能被杀死
    else if(pid == INIT_PID){
        kernel_printf("PC_kill: init process can not be killed!\n");
        return 1;
    }

    //进程不能杀死自身
    else if(pid == current_task->pid){
        kernel_printf("PC_kill: current task can not be killed!\n");
        return 1;
    }

    disable_interrupts();

    //通过进程pid检查进程是否存在
    if(!pid_check(pid)){
        kernel_printf("PC_kill: pid not found!\n");
        enable_interrupts();
        return 1;
    }

    task_struct * task;
    //在所有进程链表中找到进程结构
    task = find_in_tasks(pid);
    
    //存在bug
    if(task == 0){
        kernel_printf("PC_kill: pid found but task_struct not found!\n");
        enable_interrupts();
        return 1;
    }

    //改变进程信息
    task->state = TASK_TERMINAL;
    remove_sched(task);
    add_terminal(task);
    
    // if(task->files != 0){
    //     task_files_delete(task);
    // }

    // if(task->mm != 0){
    //     mm_delete(task->mm);
    // }

    //释放pid
    pid_free(pid);
    update_pro_map();
    enable_interrupts();
    return 0;
}

//去除参数前空格
char * cut_blank(char * str){
    int index = 0;
    char * temp = str;
    while(*temp == ' '){
        index++;
        temp++;
    }
    return (str + index);
}

//得到每个参数
//返回下下个参数的起始偏移量
int get_each_argv(char * src, char * dst, char c){
    int index = 0;

    while(src[index] && src[index] != c){
        dst[index] = src[index];
        index++;
    }

    dst[index] = 0;
    return index;
}

//字符串转整型
int char_to_int(char * str){
    int sum = 0;
    while(*str != 0){
        sum = sum * 10 + *str - '0';
        str++;
    }
    return sum;
}

//内核线程统一入口
int kernel_proc(unsigned int argc, void * argv){

    kernel_printf("\n<<<<<<<<<<<<<<<kernel_proc>>>>>>>>>>>>>>>\n");
    kernel_printf("current_task: %d\n", current_task->pid);

    // #ifdef PC_DEBUG
    //     kernel_printf("argv = %s\n", argv);
    // #endif

    #ifdef PC_DEBUG
        kernel_printf("kernel_proc: task_name = %s\n", current_task->name);
    #endif
    //loop进程，用于测试进程优先级变化
    if(kernel_strcmp(current_task->name, "loop") == 0){
        while(1);
    }

    //循环计数输出进程的动态优先级
    int cnt1 = 0;
    int cnt2 = 0;
    while(1){
        cnt1++;
        if(cnt2 == 3){
            break;
        }
        if(cnt1 == 10000000){
            kernel_printf("\ncurrent_task: %d with d_prority: %d\n", current_task->pid, current_task->dynamic_prority);
            cnt1 = 0;
            cnt2++;
        }
    }
    //进程结束
    kernel_printf("\ncurrent_task: %d with d_prority: %d ending......\n", current_task->pid, current_task->dynamic_prority);

    //进程退出
    task_exit();

    //进程退出完成，将不会进行到这里
    kernel_printf("Kernel_proc: error!\n");
    while(1);
    return 0;
}

//kernel创建进程
//新进程的入口函数相同
//成功返回0，否则返回1
int exec_kernel(void * argv, int is_wait, int is_user){
    //获得进程名
    char name[TASK_NAME_LEN];
    char * ptr;
    int next = 0;
    ptr = cut_blank((char *)argv);
    next = get_each_argv(ptr, name, ' ');

    //获得静态优先级
    char pro[32];
    ptr += next;
    ptr = cut_blank(ptr);
    next = get_each_argv(ptr, pro, ' ');
    int s_prority = char_to_int(pro);

    //创建进程
    int res;
    pid_t new_pid;

    kernel_printf("s_prority = %d\n", s_prority);

    if(!is_user){
        //内核线程，新进程入口为kernel_proc函数
        res = task_create(name, s_prority, (void *)kernel_proc, 1, name, &new_pid, 0);
    }
    if(res != 0){
        kernel_printf("Exec_kernel: task created failed!\n");
        return 1;
    }

    //是否等待子进程
    if(is_wait){
        wait_pid(new_pid);
    }
    return 0;
}

void task_exit(){
    //idle进程退出
    if(current_task->pid == IDLE_PID){
        kernel_printf("Task_exit: idle process is exitting!\n");
        while(1);
    }

    //init进程退出
    if(current_task->pid == INIT_PID){
        kernel_printf("Task_exit: init process is exitting!\n");
        while(1);
    }

    //清理task结构信息
    // if(current_task->files != 0){
    //     task_files_delete(current_task);
    // }
    // if(current_task->mm != 0){
    //     mm_delete(current_task->mm);
    // }

    //中断关闭
    asm volatile (      
        "mfc0  $t0, $12\n\t"
        "ori   $t0, $t0, 0x02\n\t"
        "mtc0  $t0, $12\n\t"
        "nop\n\t"
        "nop\n\t"
    );

    current_task->state = TASK_TERMINAL;

    //唤醒父进程函数
    wakeup_parent();
    
    //更新动态优先级
    update_dynamic_prority();

    //更新优先级位图
    update_pro_map();
    //更新是否改变优先级
    update_is_changed();

    #ifdef PC_DEBUG
        kernel_printf("PC_exit: prepare to find next task\n");
    #endif

    //调用调度算法，选取下一个要运行的进程
    task_struct * next;
    next = find_next_task();
    
    #ifdef PC_DEBUG
        kernel_printf("PC_exit: next task pid = %d\n", next->pid);
    #endif

    // if(next->mm != 0){
    //     activate_mm(next);
    // }

    remove_sched(current_task);
    add_terminal(current_task);
    pid_free(current_task->pid);
    //更新优先级位图
    update_pro_map();
    current_task = next;

    //调用汇编代码，加载新的进程上下文信息
    switch_ex(&(current_task->context));

    //进程退出完成，将不会进行到这里
    kernel_printf("Task_exit: error!");
}

//唤醒父进程
//如果父进程处于等待队列，则将其从中删除并加入调度队列
void wakeup_parent(){
    task_struct * parent;
    pid_t ppid = current_task->ppid;
    parent = find_in_wait(ppid);
    if(parent == 0){
        kernel_printf("Wakeup_parent: parent not found!\n");
    }
    #ifdef PC_DEBUG
        //kernel_printf("wakeup: parent pid = %d\n", parent->pid);
    #endif
    
    //父进程在等待
    if(parent != 0){
        remove_sched(parent);
        add_sched(parent);
        add_pro_map(parent);
        parent->state = TASK_READY;
    }
}

//等待子进程
//停止当前进程并放入等待队列，通过调度算法选取下一进程
void wait_pid(pid_t pid){
    disable_interrupts();

    //检查等待进程是否退出
    task_struct * next;
    next = wait_check(pid);
    if(next == 0){
        enable_interrupts();
        return;
    }

    if(next->ppid != current_task->pid){
        enable_interrupts();
        return;
    }
    enable_interrupts();

    //中断关闭
    asm volatile (     
        "mfc0  $t0, $12\n\t"
        "ori   $t0, $t0, 0x02\n\t"
        "mtc0  $t0, $12\n\t"
        "nop\n\t"
        "nop\n\t"
    );

    current_task->state = -(int)pid;
    #ifdef PC_DEBUG
        kernel_printf("Wait_pid: current_pid = %d wait_pid = %d\n", current_task->pid, pid);
    #endif
    //更新动态优先级
    update_dynamic_prority();

    //更新优先级位图
    update_pro_map();
    //更新是否改变优先级
    update_is_changed();

    #ifdef PC_DEBUG
        kernel_printf("Wait_pid: prepare to find next task\n");
    #endif
    //调用调度算法，选取下一个要运行的进程
    task_struct * next_sched;
    next_sched = find_next_task();
    
    //激活地址空间
    // if(next_sched->mm != 0){
    //     activate_mm(next);
    // }
    #ifdef PC_DEBUG
        kernel_printf("Wait_pid: next task pid = %d\n", next->pid);
    #endif
    //将当前进程从调度链表中移除，放入等待链表
    remove_sched(current_task);
    //更新优先级位图
    update_pro_map();
    add_wait(current_task);

    //加载新进程的上下文信息
    task_struct * curr_sched;
    curr_sched = current_task;
    current_task = next_sched;
    switch_wa(&(next_sched->context), &(curr_sched->context));

    //被唤醒从这里执行
    kernel_printf("Wait_pid: task wake with pid = %d\n", current_task->pid);
}

task_struct * wait_check(pid_t pid){
    struct list_head * pos;
    task_struct * next;
    task_struct * ret = 0;

    //遍历所有进程链表
    list_for_each(pos, &tasks){
        next = container_of(pos, task_struct, list);
        if(next->pid == pid && next->state != TASK_TERMINAL){
            ret = next;
            break;
        }
    }
    return ret;
}
