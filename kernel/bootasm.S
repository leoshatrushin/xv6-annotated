# See https://github.com/palladian1/xv6-annotated/blob/main/boot.md
# .S - assembled by GNU assembler '(g)as', can use C preprocessor directives like #include and #define
#include "asm.h"
#include "memlayout.h"
#include "mmu.h"

# xv6 is portable across anything following the PC platform set of hardware standards
# - motherboard (CPU, memory, graphics, and I/O controller chips, busses)
# - buses adhere to PCI, USB, etc. so devices are portable across vendors
# - memory connected to CPU via address, data and control wires (e.g. read/write)
# - I/O devices - keyboard, screen, CD-ROM, disk hardware, network card which CPU can interact with
# - CPU implements x86 ISA

# Power button executes BIOS firmware pre-installed on the motherboard in a ROM chip
# Nowaways use UEFI loaded from flash memory
# BIOS
# - initializes hardware so the computer looks like a traditional standard PC
# - loads boot sector (first 512 bytes of boot disk) into memory at physical address 0x7c00
# - mov %ip, 0x7c00
# - jmp 0x7c00
# - guarantees %cs set to 0
# - now executing in 16-bit real mode
# This bootloader fits in ~470 bytes, so do not have to load a secondary bootloader
# Can fit because it makes the simplifying assumption that the kernel is written to boot disk contiguously
# at sector 1 - more commonly kernels are stored in ordinary file systems (possibly uncontiguously), or
# loaded over a network
# These complications require the boot loader to be able to drive a variety of disk and network controllers
# and understand various file systems and network procotols
# i.e. it must be a mini-OS
# Real bootstrap bootloaders also often rely on the less space-constrained BIOS for disk access rather
# than trying to drive the disk itself
# UEFI avoids this by allowing the PC to read a larger bootloader from disk

# Summary
# 1) disable interrupts
# 2) set up the GDT and segment registers so we can ignore segmentation hardware
# 3) set up a stack
# 4) switch CPU from 16-bit real mode to 32-bit protected mode
# 5) call bootmain in bootmain.c

# Modern x86
# - 8 general-purpose 32-bit registers: %eax, %ecx, %edx, %ebx, %esi, %edi, %esp, %ebp
# - %eip instruction pointer
# - 8 80-bit floating point registers (not used by xv6)
# - special-purpose registers
#   - control registers: %cr0, %cr2, %cr3, %cr4
#   - debug registers: %dr0, %dr1, %dr2, %dr3 (not used by xv6)
#   - segment registers: %cs (code), %ds (data), %ss (stack), %es, %fs, %gs
#   - global and local descriptor table pseudo-registers: %gdtr, %ldtr

# Processor is in real mode, simulating an The 1981 IBM PC Intel 8088 processor
# real mode addressing
# - 20-bit address space
# - 16-bit segment registers, 16-bit offsets
# - add segment register * 16 + offset to get linear address
.code16                       # Assemble for 16-bit mode
.globl start                  # Create global symbol, Makefile uses 'ld -e start' to set the entry point
start:
  # BIOS enabled interrupts; disable:
  # e.g. press a special key to interrupt BIOS and pick a disk to boot from
  # e.g. move mouse in BIOS menu
  # e.g. disk interrupts
  # clears IF flag in %eflags register
  cli

  # Zero data segment registers DS, ES, and SS.
  # BIOS guarantees %cs code segment set to 0
  xorw    %ax,%ax             # Set %ax to zero
  movw    %ax,%ds             # -> Data Segment
  movw    %ax,%es             # -> Extra Segment
  movw    %ax,%ss             # -> Stack Segment

  # Digression on I/O
  # - 'in' and 'out' instructions read/write values from device addresses called I/O ports
  # hardware implementation is essentially the same as reading/writing memory
  # early x86 processors just had an extra address line - 0 means I/O port, 1 means memory
  # each hardware device monitors these lines for reads and writes to its assigned range of I/O ports
  # - allows software configuration, status examination, commands
  # - e.g. command disk interface hardware to read/write sectors on disk
  # Modern x86 uses 'memory-mapped I/O' for high-speed devices such as network, disk and graphics controllers
  # No separate I/O instructions - devices are mapped into the memory address space
  # But 'in' and 'out' still linger for backwards compatibility, as do hardware devices that use them
  # e.g. the IDE disk controller used by xv6

  # Real mode address may sometimes generate 21-bit addresses
  # By default physical address line A20 (21st address bit) is tied to zero so that the first PCs 
  # with 2 MB would run software that assumed 1 MB
  # Undo that by setting the 2nd bit of keyboard controller's output port high
seta20.1:
  inb     $0x64,%al
  testb   $0x2,%al
  jnz     seta20.1                # Wait for not busy

  movb    $0xd1,%al               # 0xd1 -> port 0x64
  outb    %al,$0x64

seta20.2:
  inb     $0x64,%al
  testb   $0x2,%al
  jnz     seta20.2                # Wait for not busy

  movb    $0xdf,%al               # 0xdf -> port 0x60
  outb    %al,$0x60

  # Use a bootstrap GDT that makes virtual addresses map directly to physical addresses so that the
  # effective memory map doesn't change during the transition.
  # 32-bit protected mode addressing
  # - start with xv6 virtual ('used by program')/x86 logical ('used by CPU') address (same thing)
  #   - 20-bit segment selector, 12-bit offset, written segment:offset
  # - Segmentation - translate logical address -> linear address
  #   - Segmentation hardware uses segment selector to pick one of 4 segment registers
  #   - Segment register acts as index into a GDT/segment descriptor table
  #   - GDT entry - base physical address, limit (max virtual address), permission bits
  #   - Translation compares GDT entry's Descriptor Privilege Level/ring level (0-3) agains the CPL (current)
  #   - xv6 makes almost no use of segments, using paging alone for memory virtualisation
  #   - Often the segment is implicit and a program only manipulates the offset
  #   - each segment register holds a segment selector, which is a GDT index for a segment descriptor
  #   - i.e. CS register is used to determine where IP is relative to
  #   - i.e. in real mode CS holds a value that when multiplied by 16 gives the base address of the segment
  #   - in protected mode, CS holds a GDT index that points to a segment descriptor
  # - Paging - paging hardware, if enabled, translates linear address -> physical address
  # Set up gdt so all segments have base 0 and the maximum limit of 4GB, thus identity mapping
  # i.e. segment descriptors of gdt are set up with no translation, so mapping is identity
  lgdt    gdtdesc # load GDT with value at gdtdesc address (label) defined at the end of the file

  # Switch from real to protected mode by setting Protection Enable bit of %cr0 control register
  movl    %cr0, %eax
  orl     $CR0_PE, %eax
  movl    %eax, %cr0

//PAGEBREAK!
  # Now the CPU expects the segment registers to hold segment selectors, i.e. indices into the GDT
  # Thus we nurse the processor through an evolution from 8088 to 80286 (protected mode) to 80386 (full
  # 32-bit protected mode with paging)
  #
  # ljmp segment_selector, offset
  # loads segment_selector into %cs and offset into %ip
  # cannot directly modify %cs, far jump properly synchronizes %cs and %ip to preserve control flow integrity
  # lower 3 bits of segment selector are the privilege level and table indicator bits
  ljmp    $(SEG_KCODE<<3), $start32

.code32  # Tell assembler to generate 32-bit code now.
start32:
  # Set the protected-mode data segment registers to point to the SEG_KDATA entry
  # Logical addresses now map directly to physical addresses
  movw    $(SEG_KDATA<<3), %ax    # Our data segment selector
  movw    %ax, %ds                # -> DS: Data Segment
  movw    %ax, %es                # -> ES: Extra Segment
  movw    %ax, %ss                # -> SS: Stack Segment
  movw    $0, %ax                 # Zero segments not ready for use
  movw    %ax, %fs                # -> FS
  movw    %ax, %gs                # -> GS

  # Set up the stack pointer and call into C.
  # Memory 0xA0000-0x100000 has memory regions checked by I/O devices
  # Kernel expects to be placed at 0x100000
  # Bootloader 'start' is at 0x7c00, can grow stack down from there
  movl    $start, %esp
  call    bootmain

  # If bootmain returns (it shouldn't), trigger a Bochs (like QEMU)
  # breakpoint if running under Bochs, then loop.
  # Bochs listens on port 0x8a00, can transfer control back to it
  # On real hardware, no device listens on that port
  movw    $0x8a00, %ax            # 0x8a00 -> port 0x8a00
  movw    %ax, %dx
  outw    %ax, %dx
  movw    $0x8ae0, %ax            # 0x8ae0 -> port 0x8a00
  outw    %ax, %dx
spin:
  jmp     spin

# Bootstrap GDT
.p2align 2                                # force 4 byte alignment because x86 expects so
gdt:
  SEG_NULLASM                             # null seg
  SEG_ASM(STA_X|STA_R, 0x0, 0xffffffff)   # code seg
  SEG_ASM(STA_W, 0x0, 0xffffffff)         # writable data seg

gdtdesc:
  .word   (gdtdesc - gdt - 1)             # sizeof(gdt) - 1
  .long   gdt                             # address gdt

