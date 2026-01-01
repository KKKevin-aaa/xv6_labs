        #
        # interrupts and exceptions while in supervisor
        # mode come here.
        #
        # the current stack is a kernel stack.
        # push registers, call kerneltrap().
        # when kerneltrap() returns, restore registers, return.
        #
.globl kerneltrap
.globl kernelvec
.align 4
.type kerneltrap, @function
kernelvec:
        .cfi_startproc  # Make the start of function for GDB/Debuggers
        # GDB does not use sp or fp directly;instead it defines a concept called CFA.
        # typically it is the value of SP upon function entry,all register restoration is 
        # calculated based on offsets from the CFA.

        .cfi_signal_frame       # Mark this as a "signal frame/exception frame"

        # make room to save registers.
        addi sp, sp, -256
        # Update gdb on the new Canonical Frame Address offset
        # Critical: tell gdb: the cfa(baseline SP before call) is now "current" sp + (+256)
        .cfi_def_cfa_offset 256

        # save caller-saved registers.
        sd ra, 0(sp)
        # [Critical] Tell GDB: The original 'ra' is stored at (CFA - 256)=0
        .cfi_offset ra, -256

        sd sp, 8(sp)
        .cfi_offset sp, -248

        sd gp, 16(sp)
        .cfi_offset gp, -240

        sd tp, 24(sp)
        .cfi_offset tp, -232

        sd t0, 32(sp)
        .cfi_offset t0, -224

        sd t1, 40(sp)
        .cfi_offset t1, -216

        sd t2, 48(sp)
        .cfi_offset t2, -208

        sd s0, 56(sp)
        .cfi_offset s0, -200
        # Save SPEC!This is the key to backtracing!(Additionally)

        sd a0, 72(sp)
        .cfi_offset a0, -184

        sd a1, 80(sp)
        .cfi_offset a1, -176

        sd a2, 88(sp)
        .cfi_offset a2, -168

        sd a3, 96(sp)
        .cfi_offset a3, -160

        sd a4, 104(sp)
        .cfi_offset a4, -152

        sd a5, 112(sp)
        .cfi_offset a5, -144

        sd a6, 120(sp)
        .cfi_offset a6, -136

        sd a7, 128(sp)
        .cfi_offset a7, -128


        sd t3, 216(sp)
        .cfi_offset t3, -40

        sd t4, 224(sp)
        .cfi_offset t4, -32

        sd t5, 232(sp)
        .cfi_offset t5, -24

        sd t6, 240(sp)
        .cfi_offset t6, -16

        # get the return address from the spec and store in stack
        # inform gdb current return address(Colume 64) is at 248(sp)
        csrr t0, sepc
        sd t0, 248(sp)
        .cfi_offset 64, -8
        .cfi_return_column 64


        # call the C trap handler in trap.c
        call kerneltrap

        # ... Restore registers (ld) ...
        # Note: ld does not require CFI; CFI is only a "map" for GDB.
        # CPU executes ld to restore context; GDB uses CFI to infer context.
        # restore registers.
        ld ra, 0(sp)
        # ld sp, 8(sp)
        ld gp, 16(sp)
        # not tp (contains hartid), in case we moved CPUs
        ld t0, 32(sp)
        ld t1, 40(sp)
        ld t2, 48(sp)
        ld a0, 72(sp)
        ld a1, 80(sp)
        ld a2, 88(sp)
        ld a3, 96(sp)
        ld a4, 104(sp)
        ld a5, 112(sp)
        ld a6, 120(sp)
        ld a7, 128(sp)
        ld t3, 216(sp)
        ld t4, 224(sp)
        ld t5, 232(sp)
        ld t6, 240(sp)

        addi sp, sp, 256

        .cfi_def_cfa_offset 0

        # return to whatever we were doing in the kernel.
        sret
        
        .cfi_endproc    # the end of function
