# The xv6 kernel starts executing in this file. This file is linked with
# the kernel C code, so it can refer to kernel symbols such as main().
# The boot block (bootasm.S and bootmain.c) jumps to entry below.
        
# Multiboot header, for multiboot boot loaders like GNU Grub.
# http://www.gnu.org/software/grub/manual/multiboot/multiboot.html
# Allow bootloaders to load up kernel code in a standard way
# Now replaced with Multiboot 2
#
# Using GRUB 2, you can boot xv6 from a file stored in a
# Linux file system by copying kernel or kernelmemfs to /boot
# and then adding this menu entry:
#
# menuentry "xv6" {
# 	insmod ext2
# 	set root='(hd0,msdos1)'
# 	set kernel='/boot/kernel'
# 	echo "Loading ${kernel}..."
# 	multiboot ${kernel} ${kernel}
# 	boot
# }

#include "asm.h"
#include "memlayout.h"
#include "mmu.h"
#include "param.h"

# Must be contained in the first 8192 bytes of the kernel image
.p2align 2 # force 4 byte alignment
.text
.globl multiboot_header
multiboot_header:
  # 0x1badb002 identifies a kernel, 0x2badb002 identifies a boot loader
  #define magic 0x1badb002
  #define flags 0
  .long magic
  .long flags
  .long (-magic-flags)
  # possibly other components depending on flags

# By convention, the _start symbol specifies the ELF entry point as a virtual address
# Since we haven't set up virtual memory yet, our entry point is
# the physical address of 'entry'.
.globl _start
_start = V2P_WO(entry)

# Entering xv6 on boot processor, with paging off.
# Want to set up a simple version of paging
# Bootstrap problem - need to allocate pages to hold page tables, but need page tables to use pages
# Thus we first create a two-entry page directory
# - Entry 1 - maps virtual addresses 0-4MB to physical addresses 0-4MB
# - Entry 2 - maps virtual addresses KERNBASE-KERNBASE+4MB to physical addresses 0-4MB
# One consequence is the kernel code and data have to fit in 4MB
# Two entries solve another bootstrap problem
# - The kernel is currently running in physical addresses close to 0
# - Once we enable paging and start using higher-half virtual addresses, %esp, %eip and %cr3 will
#   still point to low addresses
# - But updating them requires accessing low addresses a few more times
# - If we left out the low addresses, we'd get a page fault, and since we don't have exception handlers
#   set up yet, that would cause a double fault, which would turn into the dreaded TRIPLE FAULT - in
#   which the CPU enters an infinite loop and requires a reboot
# These entries point directly to pages instead of page tables - x86 can skip the second layer if we use
# "huge" pages of 4MB in size instead of 4KB
# In the long run, this could lead to internal fragmentation, but it does cut down on the overhead and
# allows a faster setup. We only use them for a minute while we get ready for the full paging ordeal
.globl entry
entry:
  # Enable x86 PSE (Page Size Extension) for 4MB pages
  movl    %cr4, %eax
  orl     $(CR4_PSE), %eax
  movl    %eax, %cr4
  # Set page directory
  movl    $(V2P_WO(entrypgdir)), %eax # load physical address into %cr3
  movl    %eax, %cr3
  # Turn on paging and write protection
  movl    %cr0, %eax
  orl     $(CR0_PG|CR0_WP), %eax
  movl    %eax, %cr0

  # Processor is still running at low addresses
  # Make a new kernel stack in the higher half that will be valid after we get rid of lower address mappings
  # The linker will save some space for us above the symbol 'stack' and set it up there
  # Set up the stack pointer.
  movl $(stack + KSTACKSIZE), %esp

  # Jump to main(), and switch to executing at
  # high addresses. The indirect call is needed because
  # the assembler produces a PC-relative instruction
  # for a direct jump.
  mov $main, %eax
  jmp *%eax

# reserve space for the stack in bss
.comm stack, KSTACKSIZE
