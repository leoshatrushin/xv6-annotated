// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend); // compiler assumes implicit "extern" for each function declaration
extern char end[]; // first address after the kernel, loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct { // unnamed struct type
  struct spinlock lock;
  int use_lock; // in the early stages of the kernel we only use a single CPU and interrupts are disabled
                // plus locks add overhead and acquire() needs to call mycpu() which we haven't defined yet
  struct run *freelist;
} kmem;
// To get a better page directory, we need to assign a page of memory for it (the current one is just loaded
// from the ELF file), a page for each page table, and a page for each mapped entry in the page tables
// Thus need bookkeeping to track which pages have already been assigned
// We use a linked list of free pages, and allocate pages by popping

// bootstrap problem - need to free pages that map all of physical memory before any are allocated
// i.e. free all memory between 'end' and PHYSTOP
// Another bootstrap problem - each page has to store the pointer to the next free page, meaning we have
// to write to the page, meaning the page must already be mapped
// The trick is that we do have some physical memory we can write to - between 'end' and 4MB
// We can free that part for now, allocate some of those pages for a fresh page directory and some pages,
// then use those pages to map the rest of physical memory, then come back later and free those pages

// Context coming in
// Bootloader set up GDT to ignore segmentation
// Entry code set up barebones paging with an entrypgdir
// Initial entrypgdir only maps first 4MB of physical memory in a huge page
// Before we set up a new one and allocate pages in it, everything has to happen in the first 4MB
// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.

// initialize lock for the free list but don't use it
// called from main() with end-4MB
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

// use lock to allocate and free pages once we have multiple CPUs, a scheduler, interrupts, etc.
// called from main() with 4MB-PHYSTOP (at this point these vaddrs map identically to paddrs)
void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

// clamps inwards to page boundaries on both ends
void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// kfree() and kalloc() allocate and free whole physical pages to be added to the current page directory
// and its page tables
// i.e. this is a page allocator, not a heap allocator
// though many heap allocator implementations use linked lists of free heap regions in the same way
void
kfree(char *v)
{
  struct run *r;

  // the only addresses we'll use above the top of physical memory are for memory-mapped I/O devices and we shouldn't be freeing those pages anyway
  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use. (virtual address)
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

