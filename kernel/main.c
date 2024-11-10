#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

static void startothers(void);
static void mpmain(void)  __attribute__((noreturn));
extern pde_t *kpgdir;
extern char end[]; // first address after kernel loaded from ELF file
extern char *__STAB_BEGIN__;
extern char *__STAB_END__;
extern char *__STABSTR_BEGIN__;
extern char *__STABSTR_END__;

// Bootstrap processor starts running C code here.
// Allocate a real stack and switch to it, first
// doing some setup required for memory allocator to work.
int
main(void)
{
  char *x = __STAB_BEGIN__;
  char * y = __STAB_END__;
  char * z = __STABSTR_BEGIN__;
  char * w = __STABSTR_END__;
  // suppress unused variable warning
  (void)x;
  (void)y;
  (void)z;
  (void)w;
  // solves another bootstrap problem around paging - need to allocate pages in order to use the rest of the
  // memory, but can't allocate those pages without first freeing the rest of the memory, which requires
  // allocating pages... This function frees the the memory between 'end' and 4MB
  kinit1(end, P2V(4*1024*1024)); // phys page allocator
  // allocates a page of memory to hold the fancy full-fledged page directory
  // sets it up with mappings for the kernel's instructions and data, all of physical memory, and I/O space
  // switches to that page directory, throwing away entrypgdir
  kvmalloc();      // kernel page table
  // detects hardware components like additional CPUs, buses, interrupt controllers, etc.
  mpinit();        // detect other processors
  // programs this CPU's local interrupt controller so it'll deliver timer interrupts, exceptions, etc.
  lapicinit();     // interrupt controller
  // sets up this CPU's kernel segment descriptors in its GDT
  // we still won't really use segmentation, but we'll at least use the permission bits
  seginit();       // segment descriptors
  // disables the ancient PIC interrupt controller that nobody has used since APIC was introduced in 1989
  picinit();       // disable pic
  // programs the I/O interrupt controller to forward interrupts from the disk, keyboard, serial port, etc.
  // each device will have to be set up to send its interrupts to the I/O APIC
  ioapicinit();    // another interrupt controller
  // initializes the console (display screen) by adding it to a table that maps device numbers to device
  // functions, with entries for reading and writing to the console
  // also sets up the keyboard to send interrupts to the I/O APIC
  consoleinit();   // console hardware
  // initializes the serial port to send an interrupt if we ever receive any data over it
  // xv6 uses this to communicate with emulators like QEMU and Bochs
  uartinit();      // serial port
  pinit();         // initializes empty process table
  // sets up IDT (interrupt descriptor table) so the CPU can find interrupt handlers to deal with exceptions
  // and interrupts
  tvinit();        // trap vectors
  // initializes the buffer cache, a linked list of buffers holding cached copies of disk data
  binit();         // buffer cache
  // sets up the file table, a global array of all open files in the system
  // there are other parts of the file system that need to be initialized, e.g. logging layer and inode layer
  // but those might require sleeping which we can only do from user mode, so we'll do that in the first
  // user process we set up
  fileinit();      // file table
  // initializes the disk controller
  // checks whether the file system disk is present (because both the kernel and bootloader are on the boot
  // disk, which is separate from the disk with user programs)
  // sets up disk interrupts
  ideinit();       // disk 
  // loads entry code for all other CPUs into memory, and runs setup process for each new CPU
  startothers();   // start other processors
  // finishes initializing page allocator by freeing memoery between 4MB and PHYSTOP
  kinit2(P2V(4*1024*1024), P2V(PHYSTOP)); // must come after startothers()
  // creates the first user process, which will run initialization steps to be done in user space
  // then starts a shell
  userinit();      // first user process
  // loads the IDT into the CPU so it's now ready to receive interrupts
  // calls scheduler(), enabling interrupts and starts scheduling processes
  // scheduler() never returns
  mpmain();        // finish this processor's setup
}

// Other CPUs jump here from entryother.S.
static void
mpenter(void)
{
  switchkvm();
  seginit();
  lapicinit();
  mpmain();
}

// Common CPU setup code.
static void
mpmain(void)
{
  cprintf("cpu%d: starting %d\n", cpuid(), cpuid());
  idtinit();       // load idt register
  xchg(&(mycpu()->started), 1); // tell startothers() we're up
  scheduler();     // start running processes
}

pde_t entrypgdir[];  // For entry.S

// Start the non-boot (AP) processors.
static void
startothers(void)
{
  extern uchar _binary_entryother_start[], _binary_entryother_size[];
  uchar *code;
  struct cpu *c;
  char *stack;

  // Write entry code to unused memory at 0x7000.
  // The linker has placed the image of entryother.S in
  // _binary_entryother_start.
  code = P2V(0x7000);
  memmove(code, _binary_entryother_start, (uint)_binary_entryother_size);

  for(c = cpus; c < cpus+ncpu; c++){
    if(c == mycpu())  // We've started already.
      continue;

    // Tell entryother.S what stack to use, where to enter, and what
    // pgdir to use. We cannot use kpgdir yet, because the AP processor
    // is running in low  memory, so we use entrypgdir for the APs too.
    stack = kalloc();
    *(void**)(code-4) = stack + KSTACKSIZE;
    *(void(**)(void))(code-8) = mpenter;
    *(int**)(code-12) = (void *) V2P(entrypgdir);

    lapicstartap(c->apicid, V2P(code));

    // wait for cpu to finish mpmain()
    while(c->started == 0)
      ;
  }
}

// The boot page table used in entry.S and entryother.S.
// Page directories (and page tables) must start on page boundaries,
// hence the __aligned__ attribute.
// PTE_PS in a page directory entry enables 4Mbyte pages.

__attribute__((__aligned__(PGSIZE))) // required by paging hardware
pde_t entrypgdir[NPDENTRIES] = { // 1024 entries of type unsigned int
  // Map VA's [0, 4MB) to PA's [0, 4MB)
  // Set pages as present, writable and 4MB in size
  [0] = (0) | PTE_P | PTE_W | PTE_PS, // C allows setting specific entries in an array, rest set to 0
  // Map VA's [KERNBASE, KERNBASE+4MB) to PA's [0, 4MB)
  // Same as [PDX(KERNBASE)] - page directory index part of virtual address
  [KERNBASE>>PDXSHIFT] = (0) | PTE_P | PTE_W | PTE_PS,
};

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

