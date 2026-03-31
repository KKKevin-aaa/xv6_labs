#include "types.h"
#include "param.h"
#include "atomic.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "defs.h"
#include "per-cpu.h"
#include "slab.h"
#include "kalloc.h"
#include "fcntl.h"
#include "vm.h"
#include "sbi.h"
#include "colors.h"
#include "utils.h"

// In trampoline.S
extern char trampoline[], sig_ret[];

//Enable COW 
#define COW

// #define PROC_TEST_TIME 
// Some Per-CPU variable to support lock-less operations.
struct cpu cpus[NCPU];
/// @brief static pcb array
extern struct proc proc[NPROC];
struct mailbox req_mailbox_locked[NCPU];
struct mailbox req_mailbox_nolock[NCPU];
static slab_cache_t * tlb_data_cache=NULL;

//Below two variables are tightly coupled.
struct timer_root pcpu_timers[NCPU];
static slab_cache_t *timer_payload_cache=NULL;

extern struct spinlock kvm_lock;
extern pagetable_t kernel_pagetable;

//Encapsulate detailed IPI metadata for the target hart.
//Atomic access guarantees state consistency for software interrupt dispatching.
volatile uint32 ipi_pending_reasons[NCPU];
volatile uint32 rcu_ack_flags[NCPU];
struct spinlock rcu_writer_lock;

//rcu's writer
void rcu_join_and_wait(){
    if(intr_get()==1)   pr_err("should disable interrupt.");
    acquire(&rcu_writer_lock);  //Server for one writer.
    int cur_cpuid=cpuid();
    for(int i=0;i<NCPU;i++){
        if(i==cur_cpuid)    continue;
        rcu_ack_flags[i]=0;     //clear first.
        asm volatile(
            "amoor.w.aq x0, %1, %0"
            : "+A"(ipi_pending_reasons[i])
            : "r"(IPI_REASON_RCU)
            : "memory"
        );
        asm volatile("fence w, o" : : : "memory");
        *(volatile uint32 *)(SSWI_BASE + i * 4) =1; //exist before acked.
    }
    uint32 rcu_acked_local[NCPU]={0}, nr_acked=0;
    while(nr_acked + 1< NCPU){
        asm volatile(".word 0x0100000f" : : : "memory");
        for(int i=0;i<NCPU;i++){
            if(i==cur_cpuid || rcu_acked_local[i])    continue;
            if(rcu_ack_flags[i] || cpus[i].proc==NULL){
                nr_acked++;
                rcu_acked_local[i]=1;
                if(nr_acked +1 >= NCPU) break;
            }
        }
    }
    release(&rcu_writer_lock);
}

__init_code void init_tlb_data_cache(void){
    tlb_data_cache=create_slab_cache("tlb_data_pool", 
        sizeof(struct tlb_shootdown_req), 8, NULL, NULL);
    if(tlb_data_cache==NULL)
        panic("init tlb_data_cache fail.Unable to establish data transport channel.");
}

__init_code void init_timer_payload_cache(void){
    timer_payload_cache=create_slab_cache("timer_payload_pool", 
        sizeof(struct timer_payload), 8, NULL, NULL);
    if(timer_payload_cache==NULL)
        panic("init timer_paylaod_cache fail.Unable to establish data transport channel.");
    //also associated with initialized static array buffer.(relarge while exhausted.)
    uint64 max_supp_len=(TIMER_RESERVED_END - TIMER_RESERVED_START ) / NCPU;
    void *new_mem=NULL;       //allocate one page for each hart initially
    char tmp_name[32]="per_cpu timer lock NO.X";
    for(int i=0;i<NCPU;i++){
        pcpu_timers[i].cur_max=PGSIZE / sizeof(struct timer_node);
        pcpu_timers[i].cur_size=0;
        pcpu_timers[i].root=(struct timer_node *)(TIMER_RESERVED_START + i *max_supp_len);
        tmp_name[27]='i';
        initlock(&pcpu_timers[i].lock, tmp_name);
        new_mem=alloc_memory(PGSIZE, GFP_ZERO);
        if(new_mem==NULL){
            pr_err("Lack of necessary memory for initialize timer_node buffer.");
            panic("OOM.");
        }
        acquire(&kvm_lock);
        if(mappages(&(struct map_context){
            .pagetable=kernel_pagetable,
            .start_va=TIMER_RESERVED_START + i * max_supp_len,
            .size=PGSIZE,
            .pa=(uint64)new_mem,
            .pt_lock=&kvm_lock,
            .xperm = PTE_R | PTE_W
        }) !=0 ){
            release(&kvm_lock);
            free_pages(new_mem, PGSIZE);
            panic("map fail.");
        }
        release(&kvm_lock);
    }
}

void tlb_shootdown_issue(pagetable_t pgdir, uint64 va, uint64 len){
    push_off();
    int cur_cpuid=cpuid();
    if(NCPU > 32)
        panic("NCPU exceed the predefined length(32), plz expand!");
    volatile enum box_state *check_pos[NCPU];
    memset(check_pos, 0, sizeof(check_pos));
    uint32 nr_send_req=0;
    int ready_quit=0;
    struct tlb_shootdown_req *shared_data=slab_alloc(tlb_data_cache);
    if(shared_data==NULL)   panic("OOM.");
    shared_data->len=len;
    shared_data->start_va=va;
    shared_data->target_pgdir=pgdir;
    struct mailbox *cur_box=NULL;
    struct proc *slap_p;        //resolve time-of-check time-of-use problem
    for(int i=0;i<NCPU;i++){
        if(i==cur_cpuid)    continue;
        asm volatile(
            "ld %0, %1\n\t"
            "fence r, rw\n\t"
            : "=r"(slap_p)
            : "m"(cpus[i].proc)
            : "memory"
        );  //No necessity to use AMO operation(casue the transitation of cache Line)
        // Shadow copy(no deference), rewrite as "m"(*cpus[i].proc) 
        // while want to deep copy("new_proc=*cpus[i].proc")
        if(slap_p==NULL)    continue;
        if(slap_p->pagetable==pgdir){     //Optimistic Concurrency Control(OCC)
            //read the statement without lock, double validate before use.
            cur_box=&req_mailbox_locked[i];
            acquire(&cur_box->lock);
            uint64 free_slot = cur_box->tail % MAX_MAIL;
            while(cur_box->reqs[free_slot].status != BOX_EMPTY){
                release(&cur_box->lock);
                // asm volatile("nop");
                asm volatile(".word 0x0100000f" : : : "memory");
                if(r_sip() & SIP_SSIP)
                    software_intr_handler_locked();
                acquire(&cur_box->lock);    //relock and check again
            }
            if(cur_box->reqs[free_slot].status != BOX_EMPTY)
                panic("Memory corrupted.");
            //Send one request to one mail(A single empty slot is enough)
            cur_box->reqs[free_slot].args=shared_data;
            cur_box->reqs[free_slot].func=do_flush_tlb;
            cur_box->reqs[free_slot].status=BOX_PENDING;
            check_pos[nr_send_req++]=&cur_box->reqs[free_slot].status;
            cur_box->tail++;    //advance.
            // trigger other hart's software interrupt
            // Writing to the offset from this physical base address triggers the hardware 
            // to precisely assert a dedicated S-mode software interrupt to the target hart.
            *(volatile uint32 *)(SSWI_BASE + i * 4)  = 1;
            release(&cur_box->lock);
        }
    }
    if(nr_send_req==0){
        slab_free((void *)shared_data);
        pop_off();
        return;
    }
    // if(sbi_send_ipi(target_mask, 0).error!=SBI_SUCCESS){
    //     //Starting hartid set as zero.
    //     panic("Tlb shootdown failed.");
    // }

    while(ready_quit==0){
        if(r_sip() & SIP_SSIP)
            software_intr_handler_locked();
        //Polling to handle incoming remote request targeted at the local hart.
        for(int i=0;i<=nr_send_req;i++){
            if(i==nr_send_req){
                ready_quit=1;
                break;
            }
            if(*check_pos[i] == BOX_PENDING)
                break;
            else if(*check_pos[i] == BOX_DONE)
                *check_pos[i]=BOX_EMPTY;    //release the slot.
        }
    }
    slab_free((void *)shared_data);
    pop_off();  //Cannot assume interrupts are enabled.
}

void tlb_shootdown_issue_nolock(pagetable_t pgdir, uint64 va, uint64 len){
    //CAS, without lock.
    volatile enum box_state *check_pos[NCPU];
    memset(check_pos, 0, sizeof(check_pos));
    uint32 nr_send_req=0;
    int ready_quit=0;
    struct tlb_shootdown_req * shared_data=slab_alloc(tlb_data_cache);
    if(shared_data==NULL)     panic("OOM.");
    shared_data->len=len;
    shared_data->start_va=va;
    shared_data->target_pgdir=pgdir;
    struct mailbox *cur_box=NULL;
    struct proc *slap_p=NULL;
    push_off();     //Inhibit preemption to ensure hartid stability during communication.
    int cur_cpuid=cpuid(), new_tail=0, free_slot;
    for(int i=0;i<NCPU;i++){
        if(i==cur_cpuid)    continue;
        //Bypass the local hart and any harts without an active process context.
        asm volatile(
            "ld %0, %1\n\t"
            "fence r, rw\n\t"
            : "=r"(slap_p)
            : "m"(cpus[i].proc)
            : "memory"
        );
        if(slap_p==NULL)      continue;
        if(slap_p->pagetable==pgdir){
            cur_box=&req_mailbox_nolock[i];
            new_tail=atomic_add_and_ret((void *)&cur_box->tail, 1);
            free_slot = (new_tail -1 ) % MAX_MAIL;
            while(cur_box->reqs[free_slot].status != BOX_EMPTY){     //spin-wait
                asm volatile(".word 0x0100000f" : : : "memory");
                if(r_sip() & SIP_SSIP)
                    software_intr_handler_nolock();
            }
            cur_box->reqs[free_slot].status=BOX_CLAIMED;
            cur_box->reqs[free_slot].args=shared_data;
            cur_box->reqs[free_slot].func=do_flush_tlb;
            check_pos[nr_send_req++]=&cur_box->reqs[free_slot].status;
            asm volatile("fence rw, w" : : : "memory");
            cur_box->reqs[free_slot].status=BOX_PENDING;
            // trigger other hart's software interrupt
            asm volatile("fence w, o" : : : "memory");
            //(make sure finish general memory writing before write to IO-device)
            *(volatile uint32 *)(SSWI_BASE + i * 4)  = 1;
        }
    }
    if(nr_send_req==0){
        slab_free((void *)shared_data);
        pop_off();
        return;
    }
    // if(sbi_send_ipi(target_mask, 0).error != SBI_SUCCESS)
    //     //Starting hartid set as zero.
    //     panic("TLB shootdown failed.");

    while(ready_quit==0){
        // Handle pending software interrupts targeted at the local hart
        // and requested by remote cores.
        if(r_sip() & SIP_SSIP)  
            software_intr_handler_nolock();
        //Polling to handle incoming remote request.
        for(int i=0;i<=nr_send_req;i++){
            if(i==nr_send_req){
                ready_quit=1;
                break;
            }
            if(*check_pos[i]==BOX_PENDING)  break;
            else if(*check_pos[i]==BOX_DONE)
                *check_pos[i]=BOX_EMPTY;
        }
    }
    slab_free((void *)shared_data);
    pop_off();
}

int timer_less_cmp(void *data, uint64 a, uint64 b){
    if(data==NULL)  return -1;
    //explain as struct timer_node
    struct timer_node *timer_base=(struct timer_node *)data;
    return timer_base[a].next_expr_time < timer_base[b].next_expr_time;
}

int timer_great_cmp(void *data, uint64 a, uint64 b){
    if(data==NULL)  return -1;
    //explain as struct timer_node
    struct timer_node *timer_base=(struct timer_node *)data;
    return timer_base[a].next_expr_time > timer_base[b].next_expr_time;
}

void timer_swap(void *data, uint64 a, uint64 b){
    if(data==NULL)  panic("NULL pointer passed in, invalid argument.");
    if(a==b)    return;
    //explain as struct timer_node
    struct timer_node *timer_base=data;
    if(!timer_base[a].data || !timer_base[b].data)
        panic("NULL pointer passed in, invalid argument.");
    int swap_idx=timer_base[a].data->heap_idx;
    timer_base[a].data->heap_idx=timer_base[b].data->heap_idx;
    timer_base[b].data->heap_idx=swap_idx;
    struct timer_node swap_timer=timer_base[a];
    timer_base[a]=timer_base[b];
    timer_base[b]=swap_timer;
}

int add_signal(int nr_ticks, void(*handler)(void), int nr_repeat){
    struct proc *p=myproc();
    push_off(); //No Interrupt, and Prevent yield, sleep or synchronous exceptions
    //which trigger a context switch.
    int cur_id=cpuid();
    struct timer_payload *new_payload=(struct timer_payload *)slab_alloc(timer_payload_cache);
    if(new_payload==NULL){
        pr_err("alloc struct timer_payload fail.");
        pop_off();
        return -1;
    }
    new_payload->handler=handler;
    new_payload->interval=nr_ticks;
    new_payload->bind_cpu=cur_id;
    set_timer_state(new_payload, TIMER_RUNNING);
    new_payload->proc_pid=p->pid;
    new_payload->proc_slot=p-proc;
    new_payload->repeat_left=nr_repeat;
    acquire(&pcpu_timers[cur_id].lock);
    if(new_payload==p->sig_head)    panic("");
    if(pcpu_timers[cur_id].cur_max == pcpu_timers[cur_id].cur_size + 1){
        void *new_mem=alloc_memory(PGSIZE, GFP_ATOMIC | GFP_ZERO);
        if(new_mem==NULL){
            pr_err("Lack of necessary memory for initialize timer_node buffer.");
            panic("OOM.");
        }
        acquire(&kvm_lock);
        uint64 max_supp_len = (TIMER_RESERVED_END - TIMER_RESERVED_START) / NCPU;
        if(mappages(&(struct map_context){
            .pagetable=kernel_pagetable,
            .start_va=TIMER_RESERVED_START + cur_id * max_supp_len + 
                    PGROUNDUP(sizeof(struct timer_node) * pcpu_timers[cur_id].cur_max),
            .size=PGSIZE,
            .pa=(uint64)new_mem,
            .pt_lock=&kvm_lock,
            .xperm = PTE_R | PTE_W
        }) !=0 ){
            release(&kvm_lock);
            release(&pcpu_timers[cur_id].lock);
            pop_off();
            free_pages(new_mem, PGSIZE);
            panic("map fail.");
        }
        release(&kvm_lock);
        sfence_vma(0, 0);
        pcpu_timers[cur_id].cur_max += PGSIZE / sizeof(struct timer_node);
    }
    int size=++pcpu_timers[cur_id].cur_size;
    struct timer_node *new_timer=&pcpu_timers[cur_id].root[size-1];
    memset(new_timer, 0, sizeof(struct timer_node));
    new_timer->data=new_payload;
    new_payload->heap_idx=size-1;
    new_timer->next_expr_time=r_stimecmp()+nr_ticks;
    //Maintain thge list registered timers of the current process.
    acquire(&p->lock);
    if(p->sig_head == NULL) p->sig_head=new_payload;
    else{
        new_payload->next=p->sig_head;
        if(new_payload==p->sig_head)    panic("");
        p->sig_head->prev=new_payload;
        p->sig_head=new_payload;
    }
    release(&p->lock);
    heap_sift_up((void *)pcpu_timers[cur_id].root, size-1, 
        size, timer_less_cmp, timer_swap);
    release(&pcpu_timers[cur_id].lock);
    pop_off();
    return 0;
}

int pause_signal(){
    struct proc *p=myproc();
    acquire(&p->lock);
    struct timer_payload *curr_timer=p->sig_head;
    enum TIMER_STATE t_state;
    while(curr_timer !=NULL){
        t_state=curr_timer->mask & TIMER_STATE_MASK;
        if(t_state != TIMER_PENDING && t_state != TIMER_RUNNING &&
            t_state != TIMER_EXPIRED){
            curr_timer=curr_timer->next;
            continue;
        }
        int bind_cpu=curr_timer->bind_cpu;
        if(bind_cpu < 0 || bind_cpu >= NCPU)
            panic("timer(%llx) bind to NO.%d CPU!(valid range: 0~%d)",
                    (uint64)curr_timer, bind_cpu, NCPU);
        release(&p->lock);      //AB-BA deadlock.
        acquire(&pcpu_timers[bind_cpu].lock);
        acquire(&p->lock);      //now holding two locks by correct order.
        //timer held maybe outdated or remove from the proc's list, double check.
        struct timer_payload *recheck_timer=p->sig_head;
        while(recheck_timer!=NULL){
            if(recheck_timer==curr_timer)     break;
            recheck_timer=recheck_timer->next;
        }
        //have removed from the proc's timer_list, no necessity to continue.
        if(recheck_timer!=curr_timer || recheck_timer->bind_cpu!=bind_cpu){
            //Migaration(puase and restart in other hart during the critical area.)
            release(&pcpu_timers[bind_cpu].lock);
            curr_timer=p->sig_head;
            continue;
        }
        t_state=curr_timer->mask & TIMER_STATE_MASK;
        if (t_state != TIMER_PENDING && t_state!= TIMER_RUNNING) {
            release(&pcpu_timers[bind_cpu].lock);
            curr_timer=curr_timer->next;
            continue;
        }
        set_timer_state(curr_timer, TIMER_PAUSE);
        int last_idx=pcpu_timers[bind_cpu].cur_size-1;
        heap_remove_idx(pcpu_timers[bind_cpu].root, curr_timer->heap_idx, 
            pcpu_timers[bind_cpu].cur_size, timer_less_cmp, timer_swap);
        curr_timer->heap_idx=-1;
        pcpu_timers[bind_cpu].root[pcpu_timers[bind_cpu].cur_size-1].data=NULL;
        pcpu_timers[bind_cpu].cur_size--;
        pcpu_timers[bind_cpu].root[last_idx].data=NULL;
        curr_timer->heap_idx=-1;
        curr_timer->bind_cpu=-1;
        release(&pcpu_timers[bind_cpu].lock);
        //remove from heap, but keep in proc's timer_list to resume future.
        curr_timer=curr_timer->next;
    }
    release(&p->lock);
    return 0;
}

uint64 kreturn(void){
    // Only if callback function end executing, the program will return here.
    // So restore the trapframe is necessary to support nested and re-entrant signal handle.
    struct proc *p=myproc();
    //Functions return void by defualt.
    acquire(&p->lock);
    struct timer_payload *check_timer=p->handling_sig;
    uint64 restore_sp=0;
    if(check_timer==NULL)   panic("Empty handling timer list during kreturn.");
    if(check_timer->repeat_left <=0){
        struct timer_payload *prev_timer=check_timer->prev, *next_timer=check_timer->next;
        if(prev_timer==check_timer || next_timer == check_timer)
            panic("");
        if(next_timer!=NULL)    next_timer->prev=prev_timer;
        if(prev_timer!=NULL)    prev_timer->next=next_timer;
        else    p->sig_head=next_timer;
        int cur_id=cpuid();
        acquire(&pcpu_timers[cur_id].lock);
        pcpu_timers[cur_id].root[check_timer->heap_idx].data=NULL;  //cut the link.
        release(&pcpu_timers[cur_id].lock);
        slab_free((void *)check_timer);
    }
    else    set_timer_state(check_timer, TIMER_RUNNING);
    restore_sp=check_timer->backup_sp;
    //pop from the handling list,no guarantee for subsequent states.
    p->handling_sig=p->handling_sig->handling_next;
    if(p->handling_sig==NULL){
        int has_pending=0;
        check_timer=p->sig_head;
        enum TIMER_STATE cur_state;
        while(check_timer!=NULL){
            cur_state = check_timer->mask & TIMER_STATE_MASK;
            if(cur_state == TIMER_PENDING || cur_state == TIMER_EXPIRED){
                has_pending=1;
                break;
            }
            check_timer=check_timer->next;
        }
        if(has_pending==0)
            p->pending_signals &= ~(1ull << SIG_MYALARM);   //Not pending signal at all.
    }
    //exit normally, no other pending signal.
    release(&p->lock);
    if(copyin(p->pagetable, (char *)p->trapframe, restore_sp,
                        sizeof(struct trapframe))==-1){
        pr_err("Unable to restore trapframe.");
        kexit(-1);
    }
    return p->trapframe->a0; 
    // Extract the syscall result from a0 to avoid clobbering by local assignments.
}