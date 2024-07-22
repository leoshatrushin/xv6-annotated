K=kernel
U=user
INC=include 

OBJS = \
	$K/bio.o\
	$K/console.o\
	$K/exec.o\
	$K/file.o\
	$K/fs.o\
	$K/ide.o\
	$K/ioapic.o\
	$K/kalloc.o\
	$K/kbd.o\
	$K/lapic.o\
	$K/log.o\
	$K/main.o\
	$K/mp.o\
	$K/picirq.o\
	$K/pipe.o\
	$K/proc.o\
	$K/sleeplock.o\
	$K/spinlock.o\
	$K/string.o\
	$K/swtch.o\
	$K/syscall.o\
	$K/sysfile.o\
	$K/sysproc.o\
	$K/trapasm.o\
	$K/trap.o\
	$K/uart.o\
	$K/vectors.o\
	$K/vm.o\

# Cross-compiling (e.g., on Mac OS X)
# TOOLPREFIX = i386-jos-elf

# Using native tools (e.g., on X86 Linux)
#TOOLPREFIX = 

# Try to infer the correct TOOLPREFIX if not set
ifndef TOOLPREFIX
TOOLPREFIX := $(shell if i386-jos-elf-objdump -i 2>&1 | grep '^elf32-i386$$' >/dev/null 2>&1; \
	then echo 'i386-jos-elf-'; \
	elif objdump -i 2>&1 | grep 'elf32-i386' >/dev/null 2>&1; \
	then echo ''; \
	else echo "***" 1>&2; \
	echo "*** Error: Couldn't find an i386-*-elf version of GCC/binutils." 1>&2; \
	echo "*** Is the directory with i386-jos-elf-gcc in your PATH?" 1>&2; \
	echo "*** If your i386-*-elf toolchain is installed with a command" 1>&2; \
	echo "*** prefix other than 'i386-jos-elf-', set your TOOLPREFIX" 1>&2; \
	echo "*** environment variable to that prefix and run 'make' again." 1>&2; \
	echo "*** To turn off this error, run 'gmake TOOLPREFIX= ...'." 1>&2; \
	echo "***" 1>&2; exit 1; fi)
endif

# If the makefile can't find QEMU, specify its path here
# QEMU = qemu-system-i386

# Try to infer the correct QEMU
ifndef QEMU
QEMU = $(shell if which qemu > /dev/null; \
	then echo qemu; exit; \
	elif which qemu-system-i386 > /dev/null; \
	then echo qemu-system-i386; exit; \
	elif which qemu-system-x86_64 > /dev/null; \
	then echo qemu-system-x86_64; exit; \
	else \
	qemu=/Applications/Q.app/Contents/MacOS/i386-softmmu.app/Contents/MacOS/i386-softmmu; \
	if test -x $$qemu; then echo $$qemu; exit; fi; fi; \
	echo "***" 1>&2; \
	echo "*** Error: Couldn't find a working QEMU executable." 1>&2; \
	echo "*** Is the directory containing the qemu binary in your PATH" 1>&2; \
	echo "*** or have you tried setting the QEMU variable in Makefile?" 1>&2; \
	echo "***" 1>&2; exit 1)
endif

CC = $(TOOLPREFIX)gcc -I$(INC)
AS = $(TOOLPREFIX)gas
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump
CFLAGS = -fno-pic -static -fno-builtin -fno-strict-aliasing -O2 -Wall -MD -ggdb -m32 -Werror -fno-omit-frame-pointer
CFLAGS += $(shell $(CC) -fno-stack-protector -E -x c /dev/null >/dev/null 2>&1 && echo -fno-stack-protector)
ASFLAGS = -m32 -gdwarf-2 -Wa,-divide
# FreeBSD ld wants ``elf_i386_fbsd''
LDFLAGS += -m $(shell $(LD) -V | grep elf_i386 2>/dev/null | head -n 1)

# Disable PIE when possible (for Ubuntu 16.10 toolchain)
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]no-pie'),)
CFLAGS += -fno-pie -no-pie
endif
ifneq ($(shell $(CC) -dumpspecs 2>/dev/null | grep -e '[^f]nopie'),)
CFLAGS += -fno-pie -nopie
endif


xv6.img: $K/bootblock $K/kernel
	dd if=/dev/zero of=xv6.img count=10000
	dd if=$K/bootblock of=xv6.img conv=notrunc
	dd if=$K/kernel of=xv6.img seek=1 conv=notrunc

$K/bootblock: $K/bootasm.S $K/bootmain.c
	$(CC) $(CFLAGS) -fno-pic -O -nostdinc -c $K/bootmain.c -o $K/bootmain.o 
	$(CC) $(CFLAGS) -fno-pic -nostdinc -c $K/bootasm.S -o $K/bootasm.o 
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7C00 -o $K/bootblock.o $K/bootasm.o $K/bootmain.o
	$(OBJDUMP) -S $K/bootblock.o > $K/bootblock.asm
	$(OBJCOPY) -S -O binary -j .text $K/bootblock.o $K/bootblock
	./sign.pl $K/bootblock

entryother: $K/entryother.S
	$(CC) $(CFLAGS) -fno-pic -nostdinc -c $K/entryother.S -o $K/entryother.o 
	$(LD) $(LDFLAGS) -N -e start -Ttext 0x7000 -o $K/bootblockother.o $K/entryother.o
	$(OBJCOPY) -S -O binary -j .text $K/bootblockother.o entryother
	$(OBJDUMP) -S $K/bootblockother.o > $K/entryother.asm

initcode: $U/initcode.S
	$(CC) $(CFLAGS) -nostdinc -c $U/initcode.S -o $U/initcode.o 
	$(LD) $(LDFLAGS) -N -e start -Ttext 0 -o $U/initcode.out $U/initcode.o
	$(OBJCOPY) -S -O binary $U/initcode.out initcode
	$(OBJDUMP) -S $U/initcode.o > $U/initcode.asm

$K/kernel: $(OBJS) $K/entry.o entryother initcode $K/kernel.ld
	$(LD) $(LDFLAGS) -T $K/kernel.ld -o $K/kernel $K/entry.o $(OBJS) -b binary initcode entryother
	$(OBJDUMP) -S $K/kernel > $K/kernel.asm
	$(OBJDUMP) -t $K/kernel | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $K/kernel.sym

$K/vectors.S: $K/vectors.pl
	./$K/vectors.pl > $K/vectors.S

ULIB = $U/ulib.o $U/usys.o $U/printf.o $U/umalloc.o

_%: %.o $(ULIB)
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $@ $^
	$(OBJDUMP) -S $@ > $*.asm
	$(OBJDUMP) -t $@ | sed '1,/SYMBOL TABLE/d; s/ .* / /; /^$$/d' > $*.sym

$U/_forktest: $U/forktest.o $(ULIB)
	# forktest has less library code linked in - needs to be small
	# in order to be able to max out the proc table.
	$(LD) $(LDFLAGS) -N -e main -Ttext 0 -o $U/_forktest $U/forktest.o $U/ulib.o $U/usys.o
	$(OBJDUMP) -S $U/_forktest > $U/forktest.asm

fs/mkfs: fs/mkfs.c include/fs.h 
	gcc -Werror -Wall -I. -o fs/mkfs fs/mkfs.c

# Prevent deletion of intermediate files, e.g. cat.o, after first build, so
# that disk image changes after first build are persistent until clean.  More
# details:
# http://www.gnu.org/software/make/manual/html_node/Chained-Rules.html
.PRECIOUS: %.o

UPROGS=\
	$U/_cat\
	$U/_echo\
	$U/_forktest\
	$U/_grep\
	$U/_init\
	$U/_kill\
	$U/_ln\
	$U/_ls\
	$U/_mkdir\
	$U/_rm\
	$U/_sh\
	$U/_stressfs\
	$U/_usertests\
	$U/_wc\
	$U/_zombie\

fs.img: fs/mkfs README $(UPROGS)
	fs/mkfs fs.img README $(UPROGS)

-include kernel/*.d user/*.d

clean: 
	rm -f *.tex *.dvi *.idx *.aux *.log *.ind *.ilg \
	*/*.o */*.d */*.asm */*.sym $K/vectors.S $K/bootblock entryother \
	initcode $U/initcode.out $K/kernel xv6.img fs.img \
	fs/mkfs .gdbinit \
	$(UPROGS)


# try to generate a unique GDB port
GDBPORT = $(shell expr `id -u` % 5000 + 25000)
# QEMU's gdb stub command line changed in 0.11
QEMUGDB = $(shell if $(QEMU) -help | grep -q '^-gdb'; \
	then echo "-gdb tcp::$(GDBPORT)"; \
	else echo "-s -p $(GDBPORT)"; fi)
ifndef CPUS
CPUS := 2
endif
QEMUOPTS = -drive file=fs.img,index=1,media=disk,format=raw -drive file=xv6.img,index=0,media=disk,format=raw -smp $(CPUS) -m 512 $(QEMUEXTRA)

qemu: fs.img xv6.img
	$(QEMU) -serial mon:stdio $(QEMUOPTS)

qemu-nox: fs.img xv6.img
	$(QEMU) -nographic $(QEMUOPTS)

.gdbinit: .gdbinit.tmpl
	sed "s/localhost:1234/localhost:$(GDBPORT)/" < $^ > $@

qemu-gdb: fs.img xv6.img .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -serial mon:stdio $(QEMUOPTS) -S $(QEMUGDB)

qemu-nox-gdb: fs.img xv6.img .gdbinit
	@echo "*** Now run 'gdb'." 1>&2
	$(QEMU) -nographic $(QEMUOPTS) -S $(QEMUGDB)

