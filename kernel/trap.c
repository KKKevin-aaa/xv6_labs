#include "types.h"
#include "param.h"
#include "atomic.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "colors.h"
#include "utils.h"

struct spinlock tickslock;
uint ticks;

//IPI relevant variables
extern struct mailbox req_mailbox[NCPU];

extern char trampoline[], uservec[];
extern void *usyscall_pa;
// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void trapinit(void) { initlock(&tickslock, "time"); }

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


void software_intr_handler(){
    //Interrupt(Unpreditable, caused by external hardware)
    acquire(&req_mailbox[cpuid()].lock);
    struct mailbox *cur_mailbox=&req_mailbox[cpuid()];
    //NOTE: for aggregate type, use {} to initialize instead "=" directly.
    struct tlb_shootdown_req *batched_tlb_req[NCPU]={NULL};
    int tlb_req_idx=0, did_flush=0;
    uint64 accu_npages=0, tlb_thresh=64;
    if(unlikely(cur_mailbox->tail < cur_mailbox->head))
        panic("Wrapped, slot index has exhausted!");
    for(int i=cur_mailbox->head;i!=cur_mailbox->tail;i++){
        if(cur_mailbox->reqs[i % MAX_MAIL].status==BOX_CLAIMED)
            panic("Unsupported now.");  //FIXME: 
        else if(cur_mailbox->reqs[i % MAX_MAIL].status!=BOX_PENDING)
            continue;
        if(cur_mailbox->reqs[i % MAX_MAIL].func==do_flush_tlb){    //request batching
            //extract valid element and concatenate valid data element.
            struct tlb_shootdown_req *tlb_req=(struct tlb_shootdown_req *)cur_mailbox->reqs[i % MAX_MAIL].args;
            if(did_flush==0){
                batched_tlb_req[tlb_req_idx++]=tlb_req;
                if(tlb_req_idx >= NCPU){
                    accu_npages += cal_flush_num(batched_tlb_req, tlb_req_idx, tlb_thresh-accu_npages);
                    if(accu_npages >= tlb_thresh){
                        tlb_flush_handler(batched_tlb_req, tlb_req_idx, 1);
                        did_flush=1;
                    }
                    else    tlb_flush_handler(batched_tlb_req, tlb_req_idx, 0);
                    tlb_req_idx=0;
                }
            }
            //Acknowledge completion to the initiator by resetting the request status.
            cur_mailbox->reqs[i % MAX_MAIL].status=BOX_DONE;
        }
        else{
            cur_mailbox->reqs[i % MAX_MAIL].func(cur_mailbox->reqs[i % MAX_MAIL].args);
            cur_mailbox->reqs[i % MAX_MAIL].status=BOX_DONE;
        }
    }
    //Clear the SSIP(Supervisor Software Interrupt Pending) bit to acknowledge
    //the interrupt and prevent re-triggerring(Infinite loops)
    asm volatile("csrc sip, %0" : : "r"(2));
    //CSR read and clear bits(read first, then &= ~mask, write back)
    cur_mailbox->head=cur_mailbox->tail;    // Index adjustment.
    release(&cur_mailbox->lock);
}

void software_intr_handler_nolock(){
    push_off();
    int cur_id=cpuid();
    struct mailbox *cur_mailbox=&req_mailbox[cur_id];
    struct tlb_shootdown_req *batched_tlb_req[NCPU]={NULL};
    int tlb_req_idx=0, did_flush=0;
    uint64 cur_tail=*(volatile uint64 *)&cur_mailbox->tail;
    uint64 accu_npages=0, tlb_thresh=64;
    if(unlikely(cur_tail < cur_mailbox->head))
        panic("Wrapped, slot index has exhausted!");
    for(uint64 i=cur_mailbox->head;i!=cur_tail;i++){
        if(cur_mailbox->reqs[i % MAX_MAIL].status!=BOX_PENDING){
            cur_mailbox->head=i;
            asm volatile("csrc sip, %0" : : "r"(2));
            return;     //stop and return early
            //(Once one request is not ready, we still have chance to back and deal the remaining)
        }
        asm volatile("fence r, rw" : : : "memory");
        if(cur_mailbox->reqs[i % MAX_MAIL].func==do_flush_tlb){    //request batching
            //extract valid element and concatenate valid data element.
            struct tlb_shootdown_req *tlb_req=(struct tlb_shootdown_req *)cur_mailbox->reqs[i % MAX_MAIL].args;
            if(did_flush==0){
                batched_tlb_req[tlb_req_idx++]=tlb_req;
                if(tlb_req_idx >= NCPU){
                    accu_npages += cal_flush_num(batched_tlb_req, tlb_req_idx, tlb_thresh-accu_npages);
                    if(accu_npages >= tlb_thresh){
                        tlb_flush_handler(batched_tlb_req, tlb_req_idx, 1);
                        did_flush=1;
                    }
                    else    tlb_flush_handler(batched_tlb_req, tlb_req_idx, 0);
                    tlb_req_idx=0;
                }
            }
            //Acknowledge completion to the initiator by resetting the request status.
            cur_mailbox->reqs[i % MAX_MAIL].status=BOX_DONE;
        }
        else{
            cur_mailbox->reqs[i % MAX_MAIL].func(cur_mailbox->reqs[i % MAX_MAIL].args);
            cur_mailbox->reqs[i % MAX_MAIL].status=BOX_DONE;
        }
    }
    //Clear the SSIP(Supervisor Software Interrupt Pending) bit to acknowledge
    //the interrupt and prevent re-triggerring(Infinite loops)
    asm volatile("csrc sip, %0" : : "r"(2));
    //CSR read and clear bits(read first, then &= ~mask, write back)
    cur_mailbox->head=cur_tail;    // Index adjustment.
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
                    if((*new_pte & PTE_V)==0){
                        pr_err("After page_fault handler, pte still invalid!");
                        setkilled(p);
                    }
                    if(scause==15 && !(*new_pte & PTE_W)){
                        pr_err("Vmfualt lied.PTE is still read-only.");
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
            else{
                // Unhandle Trap
                pte_t *invalid_pte=walk(p->pagetable, r_stval(), 0, 0);
                printf("usertrap(): unexpected scause 0x%llx pid=%d\n", r_scause(), p->pid);
                printf("            sepc=0x%llx stval=0x%llx\n", r_sepc(), r_stval());
                printf("            pagetable is %p, name is %s, pte is %llx, and pa is %llx\n", 
                    p->pagetable, p->name, *invalid_pte, PTE2PA(*invalid_pte));
                setkilled(p);
            }
        }
        else if(scause == (1ull << 63 | 1))     software_intr_handler_nolock();
        else{
            // interrupt or trap from an unknown source
            printf("scause=0x%llx sepc=0x%llx stval=0x%llx\n", scause, r_sepc(), r_stval());
            setkilled(p);
        }
    }

    if (killed(p)) kexit(-1);

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2) yield();

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
        if(which_dev==2 && myproc()!=0)     yield();
    }
    else{
        if(scause == (1ull << 63 | 1))     software_intr_handler_nolock();
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
