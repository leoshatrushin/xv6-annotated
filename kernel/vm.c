#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

// some parts of this file deals with the general paging implementation
// others handle the details of paging for processes and user code

extern char data[];  // defined by kernel.ld
// global page directory to replace entrypgdir
// pde_t - page directory entry (int)
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors as identity maps to all of memory
// Already did this in the bootloader, but we had no notion of kernel space vs user space
// Now we want to set permission flags for each segment so that user code can't access kernel code
// Can't use page directory and page table permission flags, because x86 forbids interrupts that take
// you from ring level 0 to ring level 3, so all interrupt handlers would have to be in kernel space
// with a kernel code segment selector at ring level 0
// Run once on entry on each CPU by main() - each has its own GDT
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0); // C equivalent of assembly SEG_ASM macro
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt)); // load new GDT into CPU
}

// Return PTE entry in 'pgdir' corresponding to va, which in particular contains the pa base
// alloc=1 allocates a new page table if needed, alloc=0 reports failure if a page table doesn't exist
// Software equivalent of paging hardware to be used for manual va -> pa conversion in the kernel while
// we setup the page directory
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)]; // page directory entry for va
  if(*pde & PTE_P){ // entry mapped (present)
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde)); // hardware uses pa for page table pointers, we want va
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    // i.e. undo filling pages with garbage 1 in kfree()
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. pa must be page-aligned.
// i.e. finishes the job of walkpgdir(), which can create page tables, but not pages themselves
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0) // for kalloc() and walkpgdir(), 0 is failure
      return -1;
    if(*pte & PTE_P)
      panic("remap"); // we're supposed to be allocating new pages for this address range
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Each process has its own page directory, so mappings in the lower half of the virtual address space vary
// Mappings in the higher half (where the kernel lives) will always be the same, so the kernel can always
// use the existing page directory for any process it is running
// We'll only use kpgdir when the kernel isn't running a process (kernel setup and running the scheduler)
// Thus on process creation, need to copy in all mappings the kernel expects, with proper permissions
// - memory-mapped I/O device space from 0-0x10_0000 (bootloader is also here, but we don't need it anymore)
// - kernel code
// - read-only data from 0x10_0000-'data' linker symbol in kernel.ld
// - kernel data
// - rest of physical memory up to PHYSTOP
// - more I/O devices from 0xFE00_0000 and up
// We represent each of these mappings with a struct kmap

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap { // simultaneously define struct type and variable
  void *virt; // starting virtual address
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text + rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data + extra memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
                                                      // unsigned integer overflow '0 - DEVSPACE' is fine
                                                      // signed integer overflow is undefined behaviour
};

// Set up a pgdir with page table for kernel mappings in kmap
// The kernel expects this in every pgdir
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0) // allocate page for pgdir
    return 0;
  memset(pgdir, 0, PGSIZE); // clear garbage from kfree()
  if (P2V(PHYSTOP) > (void*)DEVSPACE) // as good a place to check as any
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++) // map all entries in kmap
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      // abort - free all page tables and pgdir
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Called by main() to replace entrypgdir with kpgdir with mappings for kernel address space (upper half)
// At this point the free list still only contains pages for physical memory between 0-4MB
// the rest will have to wait until kinit2() for kpgdir to be fully set up
void
kvmalloc(void)
{
  kpgdir = setupkvm(); // setup kpgdir with all required kernel mappings
  switchkvm(); // load kpgdir into hardware
}

// Use kpgdir as the CPU's page directory, for when no process is running
void
switchkvm(void)
{
  lcr3(V2P(kpgdir)); // page directory stored in %cr3 control register
}

// Digression on user processes
// fork()
// - copy virtual memory space (page directory) - copyuvm()
// exec()
// - allocate a new page directory - setupkvm()
// - grow virtual memory space allocated in it to required size - allocuvm() and deallocuvm()
// - load program into memory in the new page directory - loaduvm()
// - skip a page, leaving it mapped but user-inaccessible, the next page becomes the process's stack - user
//   programs that blow their stack will trigger a page fault or GPF instead of overwriting - clearpteu
// - copy some arguments into the stack - copyout() copies data into a page in a page directory
// - switch to the new page directory - switchuvm()
// - get rid of the old page directory - freevm()
// - one edge case - running the first process - inituvm() sets up the first process's page directory

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  // x86 uses a TSS (Task State Segment) to keep track of process state (registers, privilege)
  // TR (Task Register) points to TSS segment descriptor in GDT
  // Used to keep track of where kernel left off, and when interrupts or syscalls change the running process

  pushcli(); // ensure updating TSS is atomic
  // initialize TSS segment descriptor in GDT
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts, // task state
                                sizeof(mycpu()->ts)-1, 0); // ring 0
  mycpu()->gdt[SEG_TSS].s = 0; // system segment flag, not application
  // update task state
  // store segment selector and stack pointer in task state, similar to bootloader and seginit()
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 (I/O privilege level) in eflags *and* iomb (I/O map base address) beyond the tss segment
  // limit forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3); // load TSS segment selector into TR
  lcr3(V2P(p->pgdir)); // switch to process's address space (load process page directory)
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
// Similar to loaduvm(), but instead of loading program code from disk, it copies it from memory
// Put 'sz' bytes from 'init' in address 0 of process's 'pgdir'
// Simple because we only call it for programs less than 1 page in size, so there's no looping over pages
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE); // clear garbage from kfree()
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U); // put in pgdir at address 0
  memmove(mem, init, sz); // copy code from init into new page
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
// Loads program from a file into memory at virtual address 'addr' using page directory 'pgdir'
// The part we want to read has size 'sz' and starts at position 'offset' in the file
// For now, know files are represented in xv6 as 'struct inode's and we can read from them using readi()
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned"); // since we're gonna run the program from this code
  for(i = 0; i < sz; i += PGSIZE){ // iterate over pages to be filled
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist"); // i.e. page table (and pages) should exist
    pa = PTE_ADDR(*pte); // get page's physical address
    // read from the file one page at a time
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    // readi takes a pointer to an inode, a kernel virtual address, file location, and segment size
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// reverse of deallocuvm() - allocate pages with kmalloc() instead of freeing with kfree()
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

 // must not grow process size into the region where it could access kernel memory
 // otherwise it might read or modify arbitrary physical memory
  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    // for loop easier than deallocuvm() because we know pages aren't mapped
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    // now have a page, but it's not yet mapped in the page directory
    // also might fail because it allocates pages for page tables
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// Shrink process virtual memory space by deallocating user pages from
// 'pgdir' to bring the process size from 'oldsz' to 'newsz'.  oldsz
// and newsz need not be page-aligned. if newsz is less than oldsz,
// does nothing.  oldsz can be larger than the actual process size.  
// Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz); // start with first page above newsz
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte) // entire page table doesn't exist
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE; // va of next pde after the one for a, PGSIZE added in loop
    else if((*pte & PTE_P) != 0){ // page table exists and page allocated
      // free page
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      // clear page table entry
      *pte = 0;
    }
  }
  return newsz;
}

// Free all pages in user space, all page tables, and 'pgdir'
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  // free all pages in user space
  deallocuvm(pgdir, KERNBASE, 0);
  // free page tables
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){ // page table exists
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  // free page directory
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
// Takes 'pgdir' and a user virtual address 'uva' and clears the "user-accessible" flag on the page
// Used to create an inaccessible page below a new process's stack to guard against stack overflows by
// causing a page fault instead of silently overwriting memory
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table and virtual address space size, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0) // set up new page directory and take care of kernel half of address space
    return 0;
  for(i = 0; i < sz; i += PGSIZE){ // pages in user half of parent process address space from 0 to sz
    // want to copy a page from parent's virtual address i to the child's address i
    // (may map to different physical addresses)
    // must figure out the corresponding kernel virtual address for parent's i
    // use walkpgdir() to get pte, then get the page's physical address
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0) // allocate page for child process
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE); // copy everything from parent page to child page
    // put new page into child's page directory
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem); // have to free here else we can't find it later (memory leak)
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address while checking the page is present and has user permission flag
// DANGEROUS - calls walkpgdir() with no null check
// copyout(), exec(), sys_exec(), shell, init - be careful if you touch any of these
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  // no null check - bad practice because programmer has to guarantee safe usage of uva2ka(), functions
  // that call uva2ka(), etc.
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from kernel va 'p' to user address 'va' in 'pgdir'.
// Most useful when pgdir is not the current page directory. (otherwise can use memmove())
// uva2ka ensures this only works for PTE_U pages.
// exec() uses this to copy command-line arguments to the stack for a program it's about to run
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  // need to get kernel virtual address corresponding to 'va', but if data crosses a page boundary
  // it may be spread across locations in physical memory (and thus also in virtual memory)
  // each iteration gets the next kernel virtual address and copies the next chunk of data
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0); // using 'pa0' is confusing here as it's not a physical address
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len) // last page
      n = len;
    memmove(pa0 + (va - va0), buf, n); // target kernel virtual address for 'va'
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

