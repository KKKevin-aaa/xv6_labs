#include "types.h"
#include "param.h"
#include "atomic.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "per-cpu.h"
#include "slab.h"
#include "kalloc.h"
#include "sbi.h"
#include "vm.h"
#include "syscall.h"
#include "colors.h"
#include "utils.h"

struct spinlock tickslock;
uint ticks;

// NOTE: some Per-CPU variable
//IPI relevant variables
extern struct mailbox req_mailbox_locked[NCPU];
extern struct mailbox req_mailbox_nolock[NCPU];
extern volatile uint32 rcu_ack_flags[NCPU];
extern volatile uint32 ipi_pending_reasons[NCPU];
//Sigalarm relevant variables
extern struct timer_root pcpu_timers[NCPU];

//from vm.c to support adjustment on kernel-pagetable while trap.
extern pagetable_t kernel_pagetable;
extern struct spinlock kvm_lock;


//from proc.c 
extern struct proc proc[NPROC];

extern char trampoline[], uservec[], sig_ret[];
extern void *usyscall_pa;
// in kernelvec.S, calls kerneltrap().
void kernelvec();


extern int devintr();

__init_code void trapinit(void) { initlock(&tickslock, "time"); }

// set up to take exceptions and traps while in the kernel.
void trapinithart(void) { w_stvec((uint64)kernelvec); }

void do_flush_tlb(void * args){
    (void )args;
}

uint64 cal_flush_num(struct tlb_shootdown_req ** buffer, int num, uint64 threshold){
    pagetable_t cur_pg=(pagetable_t)get_pagetable();
    uint64 start_offset, npages=0;
    for(int i=0;i<num;i++){
        if(buffer[i]->target_pgdir!=cur_pg){
            pr_info("No.%d request:target pagetable is unrelated to the current one.", i);
            continue;
        }
        if(buffer[i]->len ==0 ){
            pr_info("No.%d request:len is zero.", i);
            continue;
        }
        start_offset = buffer[i]->start_va % PGSIZE;
        if(start_offset + buffer[i]->len <= PGSIZE)
            npages ++;
        else{
            uint64 remain=buffer[i]->len - (PGSIZE - start_offset);
            npages += (1 + MAX(1, remain / PGSIZE));
        }
        if(npages >= threshold) return threshold;
    }
    return npages;
}

void tlb_flush_handler(struct tlb_shootdown_req **buffer, int num, int global_flush){
    pagetable_t cur_pg=(pagetable_t)get_pagetable();
    uint64 npages=0, start_offset=0;
    if(global_flush ==0 ){
        uint64 cur_va;
        for(int i=0;i<num;i++){
            if(buffer[i]->target_pgdir != cur_pg || buffer[i]->len==0)
                continue;
            cur_va=PGROUNDDOWN(buffer[i]->start_va);
            npages=1;
            start_offset = buffer[i]->start_va % PGSIZE;
            if(start_offset + buffer[i]->len > PGSIZE){
                uint64 remain=buffer[i]->len - (PGSIZE - start_offset);
                npages += MAX(1, remain / PGSIZE);
            }
            for(int j=0;j<npages;j++){
                sfence_vma(cur_va, 0);  //disable asid
                cur_va += PGSIZE;
            }
        }
    }
    else    sfence_vma(0, 0);
}

void tlb_intr_handler(){
    //Interrupt(Unpreditable, caused by external hardware)
    acquire(&req_mailbox_locked[cpuid()].lock);
    struct mailbox *cur_mailbox=&req_mailbox_locked[cpuid()];
    //NOTE: for aggregate type, use {} to initialize instead "=" directly.
    struct tlb_shootdown_req *batched_tlb_req[NCPU]={NULL};
    int tlb_req_idx=0;
    uint64 accu_npages=0, tlb_thresh=64, cur_idx=cur_mailbox->head;
    int batch_slots[NCPU]={0};
    struct proc *p=myproc();
    pagetable_t cur_pg=(p == NULL) ? NULL : p->pagetable;
    if(unlikely(cur_mailbox->tail < cur_mailbox->head))
        panic("Wrapped, slot index has exhausted!");
    for(;cur_idx!=cur_mailbox->tail;cur_idx++){
        if(cur_mailbox->reqs[cur_idx % MAX_MAIL].status==BOX_CLAIMED)
            panic("Unsupported now.");  //FIXME: 
        else if(cur_mailbox->reqs[cur_idx % MAX_MAIL].status!=BOX_PENDING)
            panic("Lock-based queue state machine corrupted.");
        if(cur_mailbox->reqs[cur_idx % MAX_MAIL].func==do_flush_tlb){    //request batching
            //extract valid element and concatenate valid data element.
            struct tlb_shootdown_req *tlb_req=
                (struct tlb_shootdown_req *)cur_mailbox->reqs[cur_idx % MAX_MAIL].args;
            if(tlb_req->target_pgdir!=cur_pg){
                cur_mailbox->reqs[cur_idx % MAX_MAIL].status=BOX_DONE;
                continue;
            }
            batched_tlb_req[tlb_req_idx]=tlb_req;
            batch_slots[tlb_req_idx++]=cur_idx;
        }
        else{
            cur_mailbox->reqs[cur_idx % MAX_MAIL].func(cur_mailbox->reqs[cur_idx % MAX_MAIL].args);
            cur_mailbox->reqs[cur_idx % MAX_MAIL].status=BOX_DONE;
        }
    }
    if(tlb_req_idx !=0 ){
        accu_npages += cal_flush_num(batched_tlb_req, tlb_req_idx, tlb_thresh - accu_npages);
        if(accu_npages >= tlb_thresh){
            tlb_flush_handler(batched_tlb_req, tlb_req_idx, 1);
        }
        else    tlb_flush_handler(batched_tlb_req, tlb_req_idx, 0);
        asm volatile("fence rw, w": : : "memory");
        for(int i=0;i<tlb_req_idx;i++){
            cur_mailbox->reqs[batch_slots[i] % MAX_MAIL].status=BOX_DONE;
        }
    }

    cur_mailbox->head=cur_idx;    // Index adjustment.
    release(&cur_mailbox->lock);
}

void tlb_intr_handler_nolock(){
    push_off();
    int cur_cpuid=cpuid();
    struct mailbox *cur_mailbox=&req_mailbox_nolock[cur_cpuid];
    struct tlb_shootdown_req *batched_tlb_req[NCPU]={NULL};
    int tlb_req_idx=0;
    uint64 cur_tail=*(volatile uint64 *)&cur_mailbox->tail;
    uint64 accu_npages=0, tlb_thresh=64, cur_idx=cur_mailbox->head;
    struct proc *p=myproc();
    pagetable_t cur_pg=(p ==NULL) ? NULL : p->pagetable;
    int batch_slots[NCPU]={0};
    if(unlikely(cur_tail < cur_mailbox->head))
        panic("Wrapped, slot index has exhausted!");
    for(;cur_idx!=cur_tail;cur_idx++){
        if(cur_mailbox->reqs[cur_idx % MAX_MAIL].status!=BOX_PENDING){
            break;     //stop and return early
            //(Once one request is not ready, we still have chance to back and deal the remaining)
        }
        asm volatile("fence r, rw" : : : "memory");
        if(cur_mailbox->reqs[cur_idx % MAX_MAIL].func==do_flush_tlb){    //request batching
            //extract valid element and concatenate valid data element.
            struct tlb_shootdown_req *tlb_req=
                (struct tlb_shootdown_req *)cur_mailbox->reqs[cur_idx % MAX_MAIL].args;
            if(tlb_req->target_pgdir !=cur_pg){
                cur_mailbox->reqs[cur_idx % MAX_MAIL].status=BOX_DONE;
                continue;       //Got the outdated info, false wakeup.
            }
            batched_tlb_req[tlb_req_idx]=tlb_req;
            //Acknowledge completion to the initiator by resetting the request status.
            batch_slots[tlb_req_idx++]=cur_idx;
        }
        else{
            cur_mailbox->reqs[cur_idx % MAX_MAIL].func(cur_mailbox->reqs[cur_idx % MAX_MAIL].args);
            cur_mailbox->reqs[cur_idx % MAX_MAIL].status=BOX_DONE;
        }
    }
    if(tlb_req_idx !=0 ){
        accu_npages += cal_flush_num(batched_tlb_req, tlb_req_idx, tlb_thresh - accu_npages);
        if(accu_npages >= tlb_thresh){
            tlb_flush_handler(batched_tlb_req, tlb_req_idx, 1);
        }
        else    tlb_flush_handler(batched_tlb_req, tlb_req_idx, 0);
        asm volatile("fence rw, w": : : "memory");
        for(int i=0;i<tlb_req_idx;i++){
            cur_mailbox->reqs[batch_slots[i] % MAX_MAIL].status=BOX_DONE;
        }
    }

    cur_mailbox->head=cur_idx;    // Index adjustment.
    pop_off();
}

void timer_intr_handler(void){
    uint64 ref_time=r_stimecmp();
    int cur_id=cpuid();     //Interrupts are disabled.
    int slot=0, exist_handling=0;
    void *del_data=NULL;
    acquire(&pcpu_timers[cur_id].lock);
    if(pcpu_timers[cur_id].cur_size==0 || pcpu_timers[cur_id].root==NULL){
        release(&pcpu_timers[cur_id].lock);
        return;
        //No signals or alarms registered for the current process.
    }
    struct timer_node *cur_root=pcpu_timers[cur_id].root;
    //check if false wakeup
    if(cur_root->next_expr_time > ref_time){
        release(&pcpu_timers[cur_id].lock);
        return;
    }
    while(1){
        if(cur_root->next_expr_time > ref_time || cur_root->data==NULL ||
            pcpu_timers[cur_id].cur_size==0 || exist_handling==1)
            break;
        slot=cur_root->data->proc_slot;
        if(slot < 0 || slot >=NPROC)  panic("Invalid timer's proc_slot.");
        acquire(&proc[slot].lock);
        if(proc[slot].pid != cur_root->data->proc_pid || proc[slot].state==PROC_UNUSED
            || proc[slot].state==PROC_ZOMBIE){
            del_data=cur_root->data;
            heap_remove_idx((void *)cur_root, 0, pcpu_timers[cur_id].cur_size,
                            timer_less_cmp, timer_swap);
            ((struct timer_payload *)del_data)->heap_idx=-1;
            pcpu_timers[cur_id].root[pcpu_timers[cur_id].cur_size-1].data=NULL;
            slab_free(del_data);
            pcpu_timers[cur_id].cur_size--;
            release(&proc[slot].lock);
            continue;
            //Timer's proc reclaimed, should reclaim this invalid timer.
        }
        enum TIMER_STATE cur_state= cur_root->data->mask & TIMER_STATE_MASK;
        switch(cur_state){
            case TIMER_HANDLING:
                exist_handling=1;
                break;
            case TIMER_EXPIRED:case TIMER_PAUSE:
                pr_err("Current statement shouldn't within the timer_heap");
                break;
            case TIMER_PENDING:
                cur_root->next_expr_time = cur_root->data->interval + ref_time;
                heap_sift_down((void *)cur_root, 0, pcpu_timers[cur_id].cur_size,
                                timer_less_cmp, timer_swap);
                break;
            case TIMER_RUNNING:
                if(!(proc[slot].pending_signals & (1ull << SIG_MYALARM)))
                    proc[slot].pending_signals |= (1ull << SIG_MYALARM);
                // periodic timer
                if(--cur_root->data->repeat_left <= 0){     //remove from heap, but keep content.
                    set_timer_state(cur_root->data, TIMER_EXPIRED);
                    heap_remove_idx((void *)cur_root, 0, pcpu_timers[cur_id].cur_size,
                                    timer_less_cmp, timer_swap);
                    pcpu_timers[cur_id].root[pcpu_timers[cur_id].cur_size-1].data->heap_idx=-1;
                    pcpu_timers[cur_id].root[pcpu_timers[cur_id].cur_size-1].data=NULL;
                    pcpu_timers[cur_id].cur_size--;
                }
                else{
                    set_timer_state(cur_root->data, TIMER_PENDING);
                    cur_root->next_expr_time = cur_root->data->interval + ref_time;
                    //update its next expire time and don't desecend timer size.
                    heap_sift_down((void *)cur_root, 0, pcpu_timers[cur_id].cur_size,
                                    timer_less_cmp, timer_swap);
                }
                break;
            default:
                pr_err("Unsupported type. ");
                panic("");
        }
        release(&proc[slot].lock);
    }
    release(&pcpu_timers[cur_id].lock);
}

void software_intr_handler_locked(void){
    uint32 pending_reason, clear_val=0;
    //Clear the SSIP(Supervisor Software Interrupt Pending) bit,ready for next intr.
    asm volatile("csrc sip, %0" : : "r"(SIP_SSIP) : "memory");
    //CSR read and clear bits(read first, then &= ~mask, write back)

    // have turned off interrupt, safe to access cpuid.
    // Spurious interrupt: the IPI payload was already consumed by a previous 
    // handler, but the physical signal arrived with latency.
    asm volatile(
        "amoswap.w.aqrl %0, %2, %1"
        : "=&r"(pending_reason), "+A"(ipi_pending_reasons[cpuid()])
        : "r"(clear_val)
        : "memory"
    );
    //another method:
    //__atomic_exchange_n(&rcu_ack_flags[cpuid()], clear_val, __ATOMIC_SEQ_CST);
    if(pending_reason & IPI_REASON_TLB)    tlb_intr_handler();
    if(pending_reason & IPI_REASON_RCU)
        asm volatile(
            "fence rw, w \n\t"
            "sw %1, %0  \n\t"
            : "=m"(rcu_ack_flags[cpuid()])
            : "r"(1)
            : "memory"
        );
}

void software_intr_handler_nolock(void){
    uint32 pending_reason, clear_val=0;
    //Clear the SSIP(Supervisor Software Interrupt Pending) bit,ready for next intr.
    asm volatile("csrc sip, %0" : : "r"(SIP_SSIP) : "memory");
    //CSR read and clear bits(read first, then &= ~mask, write back)

    // have turned off interrupt, safe to access cpuid.
    // Spurious interrupt: the IPI payload was already consumed by a previous 
    // handler, but the physical signal arrived with latency.
    asm volatile(
        "amoswap.w.aqrl %0, %2, %1"
        : "=&r"(pending_reason), "+A"(ipi_pending_reasons[cpuid()])
        : "r"(clear_val)
        : "memory"
    );
    //another method:
    //__atomic_exchange_n(&rcu_ack_flags[cpuid()], clear_val, __ATOMIC_SEQ_CST);
    if(pending_reason & IPI_REASON_TLB)    tlb_intr_handler_nolock();
    if(pending_reason & IPI_REASON_RCU)
        asm volatile(
            "fence rw, w \n\t"
            "sw %1, %0  \n\t"
            : "=m"(rcu_ack_flags[cpuid()])
            : "r"(1)
            : "memory"
        );
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
uint64 usertrap(void) { //NOTE: already in Supervisor-mode!!!!!!!!!!
    int which_dev = 0;
    if ((r_sstatus() & SSTATUS_SPP) != 0) panic("usertrap: not from user mode");

    // send interrupts and exceptions to kerneltrap(), since we're now in the kernel.
    w_stvec((uint64)kernelvec);  //DOC: kernelvec

    struct proc *p = myproc();
    uint64 scause = r_scause();
    // save user program counter.
    p->trapframe->epc = r_sepc();

    if (r_scause() == 8) {
        // system call(Environment call from U-mode!)
        if (killed(p)) kexit(-1);
        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        p->trapframe->epc += 4;
        // an interrupt will change sepc, scause, and sstatus,
        // so enable only now that we're done with those registers.
        intr_on();

        syscall();

    } else if ((which_dev = devintr()) != 0) {//device interrupt
        // ok
    } 
    else{
        //handle page fault and check if restore.(Fix-retry, )
        if(scause == 13 || scause ==15 || scause==12){    //Synchronous Exception
            // Specific error code:13--Load page fault, 15--Store/AMO page fault, 
            //  and 12--Instruction Page Fault.
            uint64 stval=r_stval();
            if(stval<MAXVA && p!=NULL && p->mm!=NULL){
                //Perform pre-checks to avoid unnecessary page fault handling
                if(vmfault(p->rb_array, p->pagetable, r_stval(), scause) != 0){
                    //Post-fix Verification
                    pte_t *new_pte=walk(p->pagetable, stval, 0, 0);
                    if(new_pte==NULL)   setkilled(p);
                    else if((*new_pte & PTE_V)==0){
                        pr_err("After page_fault handler, pte still invalid!");
                        setkilled(p);
                    }
                    if(scause==15 && !(*new_pte & PTE_W)){
                        pr_err("Vmfualt lied.PTE is still unwritable.");
                        setkilled(p);
                    }
                    else if(scause==13 && !(*new_pte & PTE_R)){
                        pr_err("Vmfault lied.PTE is still unreadable.");
                        setkilled(p);
                    }
                    else if(scause==12 && !(*new_pte & PTE_X)){
                        pr_err("Vmfault lied.PTE is still unexecutable.");
                        setkilled(p);
                    }
                    //Trigger a TLB shootdown once the invalidation criteria are met.
                    tlb_shootdown_issue_nolock(p->pagetable, stval, PGSIZE);
                }
            }
            else{// Unhandle Trap
                pte_t *invalid_pte=walk(p->pagetable, r_stval(), 0, 0);
                printf("usertrap(): unexpected scause 0x%llx pid=%d\n", r_scause(), p->pid);
                printf("            sepc=0x%llx stval=0x%llx\n", r_sepc(), r_stval());
                printf("            pagetable is %p, name is %s, pte is %llx, and pa is %llx\n", 
                    p->pagetable, p->name, *invalid_pte, PTE2PA(*invalid_pte));
                setkilled(p);
            }
        }
        else if(scause == (1ull << 63 | SIE_SSIE))
            software_intr_handler_nolock();
        else{
            // interrupt or trap from an unknown source
            printf("scause=0x%llx sepc=0x%llx stval=0x%llx\n", scause, r_sepc(), r_stval());
            setkilled(p);
        }
    }

    if (killed(p)) kexit(-1);

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2){
        timer_intr_handler();
        yield();
    }

    //check if still holding some unreleased lock before return to userspace
    if(mycpu()->noff!=0){
        printf("FATAL: Returning to user sapce with held locks!\n");
        print_held_locks();
        panic("Usertrapret locks.\n");
    }

    prepare_return();

    // the user page table to switch to, for trampoline.S
    uint64 satp = MAKE_SATP(p->pagetable);
    // return to trampoline.S; satp value in a0.
    return satp;
}

//
// set up trapframe and control registers for a return to user space
//
void prepare_return(void) {
    struct proc *p = myproc();

    // we're about to switch the destination of traps from
    // kerneltrap() to usertrap(). because a trap from kernel
    // code to usertrap would be a disaster, turn off interrupts.
    intr_off();

    // send syscalls, interrupts, and exceptions to uservec in trampoline.S
    uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
    w_stvec(trampoline_uservec);

    // set up trapframe values that uservec will need when
    // the process next traps into the kernel.
    p->trapframe->kernel_satp = r_satp();          // kernel page table
    p->trapframe->kernel_sp = p->kstack + PGSIZE;  // process's kernel stack
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp();  // hartid for cpuid()

    // set up the registers that trampoline.S's sret will use
    // to get to user space.

    // set S Previous Privilege mode to User.
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE;  // enable interrupts in user mode
    w_sstatus(x);

    // check if pending signals to handle
    if(p->pending_signals & (1uLL << SIG_MYALARM)){
        acquire(&p->lock);
        struct timer_payload *check_timer=p->sig_head;
        enum TIMER_STATE cur_state;
        while(check_timer !=NULL){
            cur_state=check_timer->mask & TIMER_STATE_MASK;
            if(cur_state != TIMER_PENDING && cur_state != TIMER_EXPIRED){
                check_timer=check_timer->next;
                continue;
            }
            uint64 backup_sp=p->trapframe->sp - sizeof(struct trapframe);
            backup_sp &= ~0xf;      //aligned with 16-bytes.
            uint64 store_handler=(uint64)check_timer->handler;
            release(&p->lock);
            if(copyout(p->pagetable, backup_sp, (char *)p->trapframe, 
                    sizeof(struct trapframe))==-1){
                pr_err("copyout fail.Unable save trapframe before return user's mode.");
                kexit(-1);
            }
            acquire(&p->lock);
            p->trapframe->epc=store_handler;   //prepare one signal each time.
            p->trapframe->ra=TRAMPOLINE + (sig_ret - trampoline);
            p->trapframe->sp=backup_sp;
            check_timer->backup_sp=backup_sp;
            //turn off interrupt, wouldn't reback into timer_intr_handler().
            set_timer_state(check_timer, TIMER_HANDLING);
            //Insert the handling list
            if(p->handling_sig==NULL){
                p->handling_sig=check_timer;
                p->handling_sig->handling_next=NULL;
            }
            else{
                check_timer->handling_next=p->handling_sig;
                p->handling_sig=check_timer;
            }
            break;
            //Optionally, pass necessary argument into a0
            // Argument must be static, or resolved based on the info encapsulated.
        }
        p->pending_signals |= (1ull << SIG_MYALARM);
        release(&p->lock);
        //Reset pending flags while trap from SYS-sigreturn instead of current.
    }

    // set S Exception Program Counter to the saved user pc.
    w_sepc(p->trapframe->epc);
    //for sret: Use the SPP in sstatus, copy SPIE into SIE, and copy sepc to pc
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
// NOTE: Any synchronous exception encountered outside of external hardware or 
// IPI interrupt handling triggers an immediate system panic!
void kerneltrap() {
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0) panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0) panic("kerneltrap: interrupts enabled");

    if((which_dev = devintr()) != 0){
        // give up the CPU if this is a timer interrupt.
        if(which_dev==2){
            timer_intr_handler();
            if(myproc()!=0)     yield();
        }
    }
    else{
        if(scause == (1ull << 63 | SIE_SSIE))   software_intr_handler_nolock();
        else if(scause == 13 || scause==15){
            //Only permit some reversed-area, which saved for continuity access.
            uint64 stval=PGROUNDDOWN(r_stval());        //aligned first.
            if(stval >= KRESERVED_START && stval + PGSIZE < KRESERVED_END){
                uint64 new_page=(uint64)alloc_memory(PGSIZE, GFP_ZERO | GFP_NOFAIL);
                if(new_page==0){
                    pr_err("Unable alloc physical memory for kernel reversed-area.");
                    panic("kerneltrap.");
                }
                acquire(&kvm_lock);
                if(mappages(&(struct map_context){
                    .pagetable=kernel_pagetable,
                    .start_va=stval,
                    .size=PGSIZE,
                    .pa=new_page,
                    .pt_lock=&kvm_lock,
                    .xperm= PTE_R | PTE_W,
                }) !=0 ){
                    free_pages((void *)new_page, PGSIZE);
                    pr_err("Unable build map from %llx to corresponding "
                            "physical resources.", stval);
                    panic("kerneltrap.");
                }
                sfence_vma(0, 0);       //clear outdated tlb.
                release(&kvm_lock);
            }
        }
        else{
            // interrupt or trap from an unknown source
            printf("scause=0x%llx sepc=0x%llx stval=0x%llx\n", scause, r_sepc(), r_stval());
            panic("kerneltrap");
        }
    }
    // the yield() may have caused some traps.
    // so restore trap registers for use by kernelvec.S's sepc instruction.
    w_sepc(sepc);
    w_sstatus(sstatus);
}

void clockintr() {
    if (cpuid() == 0) {
        acquire(&tickslock);
        ticks++;
        wakeup(&ticks);
        ((struct usyscall *)usyscall_pa)->ticks=ticks;
        release(&tickslock);
    }

    // ask for the next timer interrupt. this also clears
    // the interrupt request. 1000000 is about a tenth
    // of a second.
    w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int devintr() {
    uint64 scause = r_scause();

    if (scause == 0x8000000000000009L) {
        // this is a supervisor external interrupt, via PLIC.
        //(Platform Level Interrupt Controller)
        // irq indicates which device interrupted.
        int irq = plic_claim();

        if (irq == UART0_IRQ) {
            uartintr();
        } else if (irq == VIRTIO0_IRQ) {
            virtio_disk_intr();
        } else if (irq) {
            printf("unexpected interrupt irq=%d\n", irq);
        }

        // the PLIC allows each device to raise at most one
        // interrupt at a time; tell the PLIC the device is
        // now allowed to interrupt again.
        if (irq) plic_complete(irq);

        return 1;
    } else if (scause == 0x8000000000000005L) {
        // timer interrupt.
        clockintr();
        return 2;
    } else {
        return 0;
    }
}
