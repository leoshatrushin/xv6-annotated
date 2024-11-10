// Boot loader.
//
// Part of the boot block, along with bootasm.S, which calls bootmain().
// bootasm.S has put the processor into protected 32-bit mode.
// bootmain() loads kernel ELF image from the disk starting at
// sector 1 and jumps to the kernel entry routine main()

#include "types.h"
#include "elf.h"
#include "x86.h"
#include "memlayout.h"

#define SECTSIZE  512

void readseg(uchar*, uint, uint);

void
bootmain(void)
{
  struct elfhdr *elf;
  struct proghdr *ph, *eph;
  void (*entry)(void);
  uchar* pa;

  elf = (struct elfhdr*)0x10000;  // scratch space

  // read 1st page off disk after sector 1
  // contains ELF header + PHT
  readseg((uchar*)elf, 4096, 0);

  if(elf->magic != ELF_MAGIC)
    return;  // let bootasm.S handle error

  // Load each program segment (ignores ph flags).
  ph = (struct proghdr*)((uchar*)elf + elf->phoff);
  eph = ph + elf->phnum;
  for(; ph < eph; ph++){
    pa = (uchar*)ph->paddr; // an actual use case of paddr
    readseg(pa, ph->filesz, ph->off); // read actual ELF segments at 'off' into paddrs in case they weren't
    if(ph->memsz > ph->filesz)
      // load block bytes into string - faster than naive memset?
      // maybe even faster with stosl
      stosb(pa + ph->filesz, 0, ph->memsz - ph->filesz); // 0-fill
  }

  // Call the entry point from the ELF header.
  // Does not return!
  // paddr = 0x100000
  // By convention, paddr specified by "_start" symbol
  // The kernel is compiled and linked to expect to find itself at high virtual addresses starting
  // at 0x80100000
  // Thus function call instructions must mention addresses like 0x801xxxxx (see kernel.asm)
  // 0x80100000 physical address and 0x00100000 virtual address are configured in kernel.ld
  // There may not be any physical memory at such a high address
  // Once the kernel starts executing, it will set up the paging hardware to map virtual addresses starting at 0x80100000 to physical addresses starting at 0x00100000
  entry = (void(*)(void))(elf->entry);
  entry();
}

// OS has not set up a disk driver and interrupts are disabled - loop until disk is ready
void
waitdisk(void)
{
  // Wait for disk ready, see ide.c
  while((inb(0x1F7) & 0xC0) != 0x40)
    ;
}

// Read a single sector (512 bytes) at offset (in sectors) into dst.
// xv6 kernel starts at sector 1
void
readsect(void *dst, uint offset)
{
  // Issue command.
  waitdisk();
  outb(0x1F2, 1);   // count = 1
  outb(0x1F3, offset); // sector number register
  outb(0x1F4, offset >> 8); // cylinder low register
  outb(0x1F5, offset >> 16); // cylinder high register
  outb(0x1F6, (offset >> 24) | 0xE0); // drive/head register
  outb(0x1F7, 0x20);  // cmd 0x20 - read sectors

  // Read data.
  waitdisk();
  // x86 instruction insl reads from a port into a string, 'l' means one long (4 bytes) at a time
  insl(0x1F0, dst, SECTSIZE/4);
}

// Read 'count' bytes at 'offset' from kernel into physical address 'pa'.
// Might copy more than asked both below and after pa if offset and count are not sector-aligned
void
readseg(uchar* pa, uint count, uint offset)
{
  uchar* epa;

  epa = pa + count; // end of part we want to read

  // Round down to sector boundary.
  // Declaring pa a uchar* allows pointer arithmetic unlike void*
  // Though gcc lets you get away with it (as with a lot of things)
  pa -= offset % SECTSIZE;

  // Translate from bytes to sectors; kernel starts at sector 1.
  offset = (offset / SECTSIZE) + 1;

  // If this is too slow, we could read lots of sectors at a time.
  // We'd write more to memory than asked, but it doesn't matter --
  // we load in increasing order.
  for(; pa < epa; pa += SECTSIZE, offset++)
    readsect(pa, offset);
}
