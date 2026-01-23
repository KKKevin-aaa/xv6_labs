
# To compile and run with a lab solution, set the lab name in conf/lab.mk
# (e.g., LAB=util).  Run make grade to test solution with the lab's
# grade script (e.g., grade-lab-util).

-include conf/lab.mk

K=kernel
U=user

OBJS = \
  $(K)/$(OBJ_DIR)/entry.o \
  $(K)/$(OBJ_DIR)/kalloc.o \
  $(K)/$(OBJ_DIR)/string.o \
  $(K)/$(OBJ_DIR)/main.o \
  $(K)/$(OBJ_DIR)/vm.o \
  $(K)/$(OBJ_DIR)/proc.o \
  $(K)/$(OBJ_DIR)/swtch.o \
  $(K)/$(OBJ_DIR)/trampoline.o \
  $(K)/$(OBJ_DIR)/trap.o \
  $(K)/$(OBJ_DIR)/syscall.o \
  $(K)/$(OBJ_DIR)/sysproc.o \
  $(K)/$(OBJ_DIR)/bio.o \
  $(K)/$(OBJ_DIR)/fs.o \
  $(K)/$(OBJ_DIR)/log.o \
  $(K)/$(OBJ_DIR)/sleeplock.o \
  $(K)/$(OBJ_DIR)/file.o \
  $(K)/$(OBJ_DIR)/pipe.o \
  $(K)/$(OBJ_DIR)/exec.o \
  $(K)/$(OBJ_DIR)/sysfile.o \
  $(K)/$(OBJ_DIR)/kernelvec.o \
  $(K)/$(OBJ_DIR)/plic.o \
  $(K)/$(OBJ_DIR)/virtio_disk.o \
  $(K)/$(OBJ_DIR)/kvm.o \
  $(K)/$(OBJ_DIR)/rbtree_impl.o \
  $(K)/$(OBJ_DIR)/mm.o \
  $(K)/$(OBJ_DIR)/slab.o 

OBJS_KCSAN = \
  $(K)/$(OBJ_DIR)/start.o \
  $(K)/$(OBJ_DIR)/console.o \
  $(K)/$(OBJ_DIR)/printf.o \
  $(K)/$(OBJ_DIR)/uart.o \
  $(K)/$(OBJ_DIR)/spinlock.o

ifdef KCSAN
OBJS_KCSAN += \
	$(K)/$(OBJ_DIR)/kcsan.o
endif

ifeq ($(LAB),lock)
OBJS += \
	$(K)/$(OBJ_DIR)/stats.o\
	$(K)/$(OBJ_DIR)/sprintf.o
endif


ifeq ($(LAB),net)
OBJS += \
	$(K)/$(OBJ_DIR)/e1000.o \
	$(K)/$(OBJ_DIR)/net.o \
	$(K)/$(OBJ_DIR)/pci.o
endif


OBJS_KCSAN = \
  $(K)/$(OBJ_DIR)/start.o \
  $(K)/$(OBJ_DIR)/console.o \
  $(K)/$(OBJ_DIR)/printf.o \
  $(K)/$(OBJ_DIR)/uart.o \
  $(K)/$(OBJ_DIR)/spinlock.o

ifdef KCSAN
OBJS_KCSAN += \
	$(K)/$(OBJ_DIR)/kcsan.o
endif

ifeq ($(LAB),lock)
OBJS += \
	$(K)/$(OBJ_DIR)/stats.o\
	$(K)/$(OBJ_DIR)/sprintf.o
endif


ifeq ($(LAB),net)
OBJS += \
	$(K)/$(OBJ_DIR)/e1000.o \
	$(K)/$(OBJ_DIR)/net.o \
	$(K)/$(OBJ_DIR)/pci.o
endif


# riscv64-unknown-elf- or riscv64-linux-gnu-
# perhaps in /opt/riscv/bin
#TOOLPREFIX = 

# Try to infer the correct TOOLPREFIX if not set
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if riscv64-unknown-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-elf-'; \
	elif riscv64-elf-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-elf-'; \
	elif riscv64-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-linux-gnu-'; \
	elif riscv64-unknown-linux-gnu-objdump -i 2>&1 | grep 'elf64-big' >/dev/null 2>&1; \
	then echo 'riscv64-unknown-linux-gnu-'; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find a riscv64 version of GCC/binutils." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

QEMU = qemu-system-riscv64
MIN_QEMU_VERSION = 7.2

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

CFLAGS = -Wall -Werror -O

ifdef LAB
LABUPPER = $(shell echo $(LAB) | tr a-z A-Z)
XCFLAGS += -DSOL_$(LABUPPER) -DLAB_$(LABUPPER)
endif

CFLAGS += $(XCFLAGS)
CFLAGS += -MD
CFLAGS += -mcmodel=medany
CFLAGS += -ffreestanding
CFLAGS += -fno-common -nostdlib
CFLAGS += -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen -fno-builtin-memset
CFLAGS += -fno-builtin-memmove -fno-builtin-memcmp -fno-builtin-log -fno-builtin-bzero
CFLAGS += -fno-builtin-strchr -fno-builtin-exit -fno-builtin-malloc -fno-builtin-putc
CFLAGS += -fno-builtin-free
CFLAGS += -fno-builtin-memcpy -Wno-main
CFLAGS += -fno-builtin-printf -fno-builtin-fprintf -fno-builtin-vprintf
CFLAGS += -I.
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)

ifeq ($(LAB),net)
CFLAGS += -DNET_TESTS_PORT=$(SERVERPORT)
endif

ifdef KCSAN
CFLAGS += -DKCSAN
KCSANFLAG = -fsanitize=thread -fno-inline
endif

ifeq ($(LAB),net)
CFLAGS += -DNET_TESTS_PORT=$(SERVERPORT)
endif

ifdef KCSAN
CFLAGS += -DKCSAN
KCSANFLAG = -fsanitize=thread -fno-inline
endif

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif

BUILD_ROOT_DIR := build

DEBUG := 0
ifeq ($(DEBUG), 2)
  CFLAGS += -DDEBUG_FORK -DDEBUG_VM -DDEBUG_KALLOC -DDEBUG_EXEC -DDEBUG_PROC -O0 -ggdb3 -gdwarf-4
  CFLAGS += -fno-omit-frame-pointer
  CFLAGS += -fno-partial-inlining -fno-ipa-cp -fno-ipa-sra
  CFLAGS += -fno-optimize-sibling-calls
  CFLAGS += -finstrument-functions
  CFLAGS := $(filter-out -O%, $(CFLAGS)) -O0
  # This flags instructs the compiler to enable profilling by inserting a call
  # to the mcount routine at the prologue of every function.
  OBJ_DIR := $(BUILD_ROOT_DIR)/debug2
else ifeq ($(DEBUG), 1)
  CFLAGS +=  -O0 -ggdb3 -gdwarf-4
  CFLAGS += -fno-omit-frame-pointer
  CFLAGS += -fno-partial-inlining -fno-ipa-cp -fno-ipa-sra
  CFLAGS += -fno-optimize-sibling-calls
  CFLAGS += -finstrument-functions
  CFLAGS := $(filter-out -O%, $(CFLAGS)) -O0
  # This flags instructs the compiler to enable profilling by inserting a call
  # to the mcount routine at the prologue of every function.
  OBJ_DIR := $(BUILD_ROOT_DIR)/debug1
else
  CFLAGS += -O2 -ggdb -gdwarf-4
  OBJ_DIR := $(BUILD_ROOT_DIR)/release
endif
LOG_FILE := ./log/qemu_output.log
ifeq ($(PIPE), 1)
LOG_SUFFIX := 2>&1 | tee $(LOG_FILE)
else
# LOG_SUFFIX := 2>&1 > $(LOG_FILE)
LOG_SUFFIX :=
endif
KERNEL_TBL := $(K)/$(OBJ_DIR)/kernel.tbl

LDFLAGS = -z max-page-size=4096

$(OBJ_DIR):
	@mkdir -p $(K)/$@
	@mkdir -p $(U)/$@

$(K)/$(OBJ_DIR)/kernel: $(OBJS) $(OBJS_KCSAN) $(K)/kernel.ld | $(OBJ_DIR)
	$(LD) $(LDFLAGS) -T $(K)/kernel.ld -o $(K)/$(OBJ_DIR)/kernel $(OBJS) $(OBJS_KCSAN)
	$(OBJDUMP) -S -l $(K)/$(OBJ_DIR)/kernel > $(K)/$(OBJ_DIR)/kernel.asm
	$(OBJDUMP) -t $(K)/$(OBJ_DIR)/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(K)/$(OBJ_DIR)/kernel.sym
$(OBJS): EXTRAFLAG := $(KCSANFLAG)

$(K)/$(OBJ_DIR)/%.o: $(K)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(EXTRAFLAG) -c -o $@ $<

$(K)/$(OBJ_DIR)/%.o: $(K)/%.S | $(OBJ_DIR)
	$(CC) -c -o $@ $<

tags: $(OBJS) | $(OBJ_DIR)
	etags kernel/*.S kernel/*.c

ULIB = $(U)/$(OBJ_DIR)/ulib.o $(U)/$(OBJ_DIR)/usys.o $(U)/$(OBJ_DIR)/printf.o $(U)/$(OBJ_DIR)/umalloc.o $(U)/$(OBJ_DIR)/regexp.o

ifeq ($(LAB),lock)
ULIB += $(U)/$(OBJ_DIR)/statistics.o
endif

$(U)/$(OBJ_DIR)/_%: $(U)/$(OBJ_DIR)/%.o $(ULIB) $(U)/user.ld | $(OBJ_DIR)
	$(LD) $(LDFLAGS) -T $(U)/user.ld -o $@ $< $(ULIB)
	$(OBJDUMP) -S -l $@ > $(U)/$(OBJ_DIR)/$*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $(U)/$(OBJ_DIR)/$*.sym

$(U)/$(OBJ_DIR)/%.o :$(U)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(EXTRAFLAG) -c -o $@ $<

$(U)/$(OBJ_DIR)/usys.S : $(U)/usys.pl | $(OBJ_DIR)
	perl $(U)/usys.pl > $(U)/$(OBJ_DIR)/usys.S

$(U)/$(OBJ_DIR)/usys.o : $(U)/$(OBJ_DIR)/usys.S | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $(U)/$(OBJ_DIR)/usys.o $(U)/$(OBJ_DIR)/usys.S

$(U)/$(OBJ_DIR)/_forktest: $(U)/$(OBJ_DIR)/forktest.o $(ULIB) | $(OBJ_DIR)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $(U)/$(OBJ_DIR)/_forktest $(U)/$(OBJ_DIR)/forktest.o $(U)/$(OBJ_DIR)/ulib.o $(U)/$(OBJ_DIR)/usys.o
	$(OBJDUMP) -S -l $(U)/$(OBJ_DIR)/_forktest > $(U)/$(OBJ_DIR)/forktest.asm

mkfs/mkfs: mkfs/mkfs.c $(K)/fs.h $(K)/param.h | $(OBJ_DIR)
	gcc $(XCFLAGS) -Wno-unknown-attributes -Werror -Wall -I. -o mkfs/mkfs mkfs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

UPROGS=\
	$(U)/$(OBJ_DIR)/_cat\
	$(U)/$(OBJ_DIR)/_echo\
	$(U)/$(OBJ_DIR)/_forktest\
	$(U)/$(OBJ_DIR)/_grep\
	$(U)/$(OBJ_DIR)/_init\
	$(U)/$(OBJ_DIR)/_kill\
	$(U)/$(OBJ_DIR)/_ln\
	$(U)/$(OBJ_DIR)/_ls\
	$(U)/$(OBJ_DIR)/_mkdir\
	$(U)/$(OBJ_DIR)/_rm\
	$(U)/$(OBJ_DIR)/_sh\
	$(U)/$(OBJ_DIR)/_stressfs\
	$(U)/$(OBJ_DIR)/_usertests\
	$(U)/$(OBJ_DIR)/_grind\
	$(U)/$(OBJ_DIR)/_wc\
	$(U)/$(OBJ_DIR)/_zombie\
	$(U)/$(OBJ_DIR)/_logstress\
	$(U)/$(OBJ_DIR)/_forphan\
	$(U)/$(OBJ_DIR)/_dorphan\
	$(U)/$(OBJ_DIR)/_sandbox\
	$(U)/$(OBJ_DIR)/_reserve_test



ifeq ($(LAB),util)
UPROGS += \
	$(U)/$(OBJ_DIR)/_sleep\
	$(U)/$(OBJ_DIR)/_sixfive\
	$(U)/$(OBJ_DIR)/_find
endif
### ENDIF


ifeq ($(LAB),syscall)
UPROGS += \
	$(U)/$(OBJ_DIR)/_attack\
	$(U)/$(OBJ_DIR)/_secret
endif

ifeq ($(LAB),lock)
UPROGS += \
	$(U)/$(OBJ_DIR)/_stats
endif

ifeq ($(LAB),traps)
UPROGS += \
	$(U)/$(OBJ_DIR)/_call\
	$(U)/$(OBJ_DIR)/_bttest\
	$(U)/$(OBJ_DIR)/_btdebugtest
endif

ifeq ($(LAB),lazy)
UPROGS += \
	$(U)/$(OBJ_DIR)/_lazytests
endif

ifeq ($(LAB),cow)
UPROGS += \
	$(U)/$(OBJ_DIR)/_cowtest
endif

ifeq ($(LAB),thread)
UPROGS += \
	$(U)/$(OBJ_DIR)/_uthread

$(U)/$(OBJ_DIR)/uthread_switch.o : $(U)/$(OBJ_DIR)/uthread_switch.S | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $(U)/$(OBJ_DIR)/uthread_switch.o $(U)/$(OBJ_DIR)/uthread_switch.S

$(U)/$(OBJ_DIR)/_uthread: $(U)/$(OBJ_DIR)/uthread.o $(U)/$(OBJ_DIR)/uthread_switch.o $(ULIB) | $(OBJ_DIR)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $(U)/$(OBJ_DIR)/_uthread $(U)/$(OBJ_DIR)/uthread.o $(U)/$(OBJ_DIR)/uthread_switch.o $(ULIB)
	$(OBJDUMP) -S -l $(U)/$(OBJ_DIR)/_uthread > $(U)/$(OBJ_DIR)/uthread.asm

ph: notxv6/ph.c
	gcc -o ph -g -O2 $(XCFLAGS) notxv6/ph.c -pthread

barrier: notxv6/barrier.c
	gcc -o barrier -g -O2 $(XCFLAGS) notxv6/barrier.c -pthread
endif

ifeq ($(LAB),pgtbl)
UPROGS += \
	$(U)/$(OBJ_DIR)/_pgtbltest
endif

ifeq ($(LAB),lock)
UPROGS += \
	$(U)/$(OBJ_DIR)/_kalloctest\
	$(U)/$(OBJ_DIR)/_bcachetest
endif

ifeq ($(LAB),fs)
UPROGS += \
	$(U)/$(OBJ_DIR)/_bigfile
endif


ifeq ($(LAB),mmap)
UPROGS += \
	$(U)/$(OBJ_DIR)/_mmaptest
endif

ifeq ($(LAB),net)
UPROGS += \
	$(U)/$(OBJ_DIR)/_nettest
endif

UEXTRA=
ifeq ($(LAB),util)
	UEXTRA += user/findtest.sh
	UEXTRA += user/sixfive.txt
	UPROGS += $(U)/$(OBJ_DIR)/_memdump
endif
ifeq ($(LAB),syscall)
	UEXTRA += user/exec.sh
endif

# Add custom text files to filesystem
UEXTRA += big.txt bigger.txt biggerer.txt huge.txt

# here | is Order-only:create if noexist and don't anything if they just become newer
# Introduce a kernel dependency to effectively force a rebuild of the assembly files.
script_tools: ./Analysis_tools/gene_addr2line.py $(K)/$(OBJ_DIR)/kernel
	python ./Analysis_tools/gene_addr2line.py $(K)/$(OBJ_DIR)/kernel.asm $(KERNEL_TBL)


fs.img: mkfs/mkfs README $(UEXTRA) $(UPROGS) $(K)/$(OBJ_DIR)/kernel script_tools | $(OBJ_DIR)
	mkfs/mkfs fs.img README $(KERNEL_TBL) $(UEXTRA) $(UPROGS)

newfs.img: 
	-mv -f fs.img fs.img.bk

# -include kernel/*.d user/*.d
ifneq ($(MAKECMDGOALS), clean)
  -include $(K)/$(OBJ_DIR)/*.d $(U)/$(OBJ_DIR)/*.d
endif

clean:
	rm -rf *.tex *.dvi *.idx *.aux *.log *.ind *.ilg *.dSYM *.zip *.pcap \
	*/*.o */*.d */*.asm */*.sym $(K)/$(BUILD_ROOT_DIR) $(U)/$(BUILD_ROOT_DIR) \
	$(K)/$(OBJ_DIR)/kernel fs.img fs.img.bk \
	mkfs/mkfs .gdbinit \
        $(U)/$(OBJ_DIR)/usys.S \
	$(UPROGS)

# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 4
endif
ifeq ($(LAB),fs)
CPUS := 1
endif

FWDPORT1 = $(shell expr `id -u` % 5000 + 25999)
FWDPORT2 = $(shell expr `id -u` % 5000 + 30999)

FWDPORT1 = $(shell expr `id -u` % 5000 + 25999)
FWDPORT2 = $(shell expr `id -u` % 5000 + 30999)

QEMUOPTS = -machine virt -bios none -kernel $(K)/$(OBJ_DIR)/kernel -m 128M -smp $(CPUS) -nographic
QEMUOPTS += -global virtio-mmio.force-legacy=false
QEMUOPTS += -drive file=fs.img,if=none,format=raw,id=x0
QEMUOPTS += -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
QEMUOPTS += -cpu rv64,zbb=true
ifeq ($(LAB),net)
QEMUOPTS += -netdev user,id=net0,hostfwd=udp::$(FWDPORT1)-:2000,hostfwd=udp::$(FWDPORT2)-:2001 -object filter-dump,id=net0,netdev=net0,file=packets.pcap
QEMUOPTS += -device e1000,netdev=net0,bus=pcie.0
endif

# makes a new fs.img
qemu: check-qemu-version newfs.img $(K)/$(OBJ_DIR)/kernel fs.img | $(OBJ_DIR)
	$(QEMU) $(QEMUOPTS) $(LOG_SUFFIX)

# runs with existing fs.img, if present
qemu-fs: check-qemu-version $(K)/$(OBJ_DIR)/kernel fs.img | $(OBJ_DIR)
ifeq ($(LAB),net)
QEMUOPTS += -netdev user,id=net0,hostfwd=udp::$(FWDPORT1)-:2000,hostfwd=udp::$(FWDPORT2)-:2001 -object filter-dump,id=net0,netdev=net0,file=packets.pcap
QEMUOPTS += -device e1000,netdev=net0,bus=pcie.0
endif

.gdbinit: .gdbinit.tmpl-riscv
	sed "s/:1234/:$(GDBPORT)/; s|kernel/kernel|$(K)/$(OBJ_DIR)/kernel|" < $^ > $@

qemu-gdb: $(K)/$(OBJ_DIR)/kernel .gdbinit fs.img | $(OBJ_DIR)
	@echo "*** Now run 'gdb' in another window." 1>&2
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB) $(LOG_SUFFIX)

ifeq ($(LAB),net)
# try to generate a unique port for the echo server
SERVERPORT = $(shell expr `id -u` % 5000 + 25099)

endif

##
##  FOR testing lab grading script
##

ifneq ($(V),@)
GRADEFLAGS += -v
endif

ifeq ($(LAB),net)
# try to generate a unique port for the echo server
SERVERPORT = $(shell expr `id -u` % 5000 + 25099)

endif

##
##  FOR testing lab grading script
##

ifneq ($(V),@)
GRADEFLAGS += -v
endif

ifeq ($(LAB),net)
# try to generate a unique port for the echo server
SERVERPORT = $(shell expr `id -u` % 5000 + 25099)

endif

##
##  FOR testing lab grading script
##

ifneq ($(V),@)
GRADEFLAGS += -v
endif

print-gdbport:
	@echo $(GDBPORT)

grade:
	@echo $(MAKE) clean
	@$(MAKE) clean || \
          (echo "'make clean' failed.  HINT: Do you have another running instance of xv6?" && exit 1)
	./grade-lab-$(LAB) $(GRADEFLAGS)

##
## FOR submissions
##

submit-check:
	@if ! test -d .git; then \
		echo No .git directory, is this a git repository?; \
		false; \
	fi
	@if test "$$(git symbolic-ref HEAD)" != refs/heads/$(LAB); then \
		git branch; \
		read -p "You are not on the $(LAB) branch.  Hand-in the current branch? [y/N] " r; \
		test "$$r" = y; \
	fi
	@if ! git diff-files --quiet || ! git diff-index --quiet --cached HEAD; then \
		git status -s; \
		echo; \
		echo "You have uncomitted changes.  Please commit or stash them."; \
		false; \
	fi
	@if test -n "`git status -s`"; then \
		git status -s; \
		read -p "Untracked files will not be handed in.  Continue? [y/N] " r; \
		test "$$r" = y; \
	fi

zipball: clean submit-check
	git archive --verbose --format zip --output lab.zip HEAD

.PHONY: zipball clean grade submit-check check-qemu-version

QEMU_VERSION := $(shell $(QEMU) --version | head -n 1 | sed -E 's/^QEMU emulator version ([0-9]+\.[0-9]+)\..*/\1/')
check-qemu-version:
	@if [ "$(shell echo "$(QEMU_VERSION) >= $(MIN_QEMU_VERSION)" | bc)" -eq 0 ]; then \
		echo "ERROR: Need qemu version >= $(MIN_QEMU_VERSION)"; \
		exit 1; \
	fi
