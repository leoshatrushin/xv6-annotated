# Digression on interrupts and exceptions
# interrupt - asynchronous, exception - synchronous
# Each interrupt and exception in the IDT is identified with a number called a vector
# some vectors from 0-31 are predefined
# vectors 32-255 are for software-defined interrupts, which are for either software interrupts or
# maskable hardware interrupts
# the processor also defines several additional interrupts that do not point to entries in the IDT
# e.g. SMI interrupt
# when the processor detects an interrupt or exception, it either executes an implicit call to a handler
# procedure or handler task

#include "mmu.h"

  # vectors.S sends all traps here.
  # since the trap handler runs in kernel mode, we need to save some process state similar to struct context
.globl alltraps
alltraps:
  # Build trap frame.
  pushl %ds
  pushl %es
  pushl %fs
  pushl %gs
  pushal # push all general purpose registers
  
  # Set up data segments, i.e. set up %ds and %es for the kernel (%cs and %ss already done by processor)
  movw $(SEG_KDATA<<3), %ax
  movw %ax, %ds
  movw %ax, %es

  # Call trap(tf), where tf=%esp, which points to everything we've pushed onto the stack
  pushl %esp
  call trap
  addl $4, %esp # ignore return value

  # Return falls through to trapret...
  # new process executes forkret() and any call to trap() return here
  # restores process state
.globl trapret
trapret:
  popal
  popl %gs
  popl %fs
  popl %es
  popl %ds
  addl $0x8, %esp  # trapno and errcode
  iret # interrupt (trap) return - close out, return to user mode, start executing user code again
