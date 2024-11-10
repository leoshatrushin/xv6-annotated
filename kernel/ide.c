// Simple PIO-based (non-DMA) IDE driver code.
// IDE device provides access to disks connected to the PC standard IDE controller
// IDE is now falling out of fashion in favor of SCSI and SATA

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

// Driver - code that manages a hardware device
// - tells device to performs operations
// - configures device to generate interrupts when done
// - handles interrupts
// can be tricky to write
// - a driver executes concurrently with the device it manages
// - device interface can be complex and poorly documented

// Modern disk drivers usually talk to the disk via DMA, but for simplicity xv6 uses port I/O
// Much slower, and requires active participation from the CPU

// inb/outb read/write a byte from a port
// Storage disks have all kinds of standardized specifications, including IDE (Integrated Drive Electronics) and ATA (Advanced Technolocy Attachment)
// ATA specs include a programmed I/O mode
// disk controller chip has primary and secondary buses for ATA PIO mode
// primary bus sends data on port 0x1F0 and has control registers on ports 0x1F1-0x1F7
// port 0x1F7 doubles as a command register and status port

// status port flags
// - bit 0 (0x01) - ERR (error)
// - bit 1 (0x02) - IDX (index, always 0)
// - bit 2 (0x04) - CORR (corrected data, always 0)
// - bit 3 (0x08) - DRQ (drive has data to transfer or is ready to receive data)
// - bit 4 (0x10) - SRV (service request)
// - bit 5 (0x20) - DF (drive fault error)
// - bit 6 (0x40) - RDY (ready, cleared when drive isn't running or after an error)
// - bit 7 (0x80) - BSY (busy, drive is in the middle of sending/receiving data)
#define SECTOR_SIZE   512
#define IDE_BSY       0x80
#define IDE_DRDY      0x40
#define IDE_DF        0x20
#define IDE_ERR       0x01

// command register commands
#define IDE_CMD_READ  0x20
#define IDE_CMD_WRITE 0x30
#define IDE_CMD_RDMUL 0xc4
#define IDE_CMD_WRMUL 0xc5

// idequeue points to the buf now being read/written to the disk.
// idequeue->qnext points to the next buf to be processed.
// You must hold idelock while manipulating queue.

static struct spinlock idelock;
static struct buf *idequeue; // queue of buffers waiting to synchronized with disk

static int havedisk1; // running with only disk 0 (boot loader and kernel) or also disk 1 (user file system)
static void idestart(struct buf*);

// Wait for IDE disk to become ready.
static int
idewait(int checkerr)
{
  int r;

  while(((r = inb(0x1f7)) & (IDE_BSY|IDE_DRDY)) != IDE_DRDY)
    ;
  if(checkerr && (r & (IDE_DF|IDE_ERR)) != 0)
    return -1;
  return 0;
}

void
ideinit(void)
{
  int i;

  initlock(&idelock, "ide");
  // tell I/O interrupt controller to forward all disk interrupts to the last CPU
  // enable IDE_IRQ interrupt on the last CPU
  ioapicenable(IRQ_IDE, ncpu - 1);
  idewait(0);

  // Check if disk 1 is present
  // A PC motherboard presents status of disk hardware on I/O port 0x1f7
  // disk 0 containing the bootloader and kernel is always present
  // xv6 make qemu-memfs configuration runs without a file system disk, storing files in memory instead
  // port 0x1f6 is used to select a drive
  // bit 4 determines whether to select disk 0 or disk 1
  // bit 5 should always be set
  // bit 6 picks the right mode we need to indicate a disk
  // bit 7 shoudl always be set
  outb(0x1f6, 0xe0 | (1<<4));
  // need to wait for disk 1 to be ready, but handle this as a special case since waitdisk() can't check
  // a specific disk for us, and because an absent disk 1 would loop forever
  for(i=0; i<1000; i++){
    if(inb(0x1f7) != 0){
      // otherwise assume disk 1 isn't present
      havedisk1 = 1;
      break;
    }
  }

  // Switch back to disk 0.
  outb(0x1f6, 0xe0 | (0<<4));
}

// Start the request for b.  Caller must hold idelock.
// i.e. read/write a buffer to/from disk
static void
idestart(struct buf *b)
{
  if(b == 0)
    panic("idestart");
  if(b->blockno >= FSSIZE) // buffer block number maximum limit
    panic("incorrect blockno");
  int sector_per_block =  BSIZE/SECTOR_SIZE; // just 1 on xv6, change if we want higher disk throughput
  int sector = b->blockno * sector_per_block;
  int read_cmd = (sector_per_block == 1) ? IDE_CMD_READ :  IDE_CMD_RDMUL; // single vs multi-sector command
  int write_cmd = (sector_per_block == 1) ? IDE_CMD_WRITE : IDE_CMD_WRMUL;

  if (sector_per_block > 7) panic("idestart");

  idewait(0);
  outb(0x3f6, 0);  // tell disk controller to generate interrupt once done by setting device control register
  outb(0x1f2, sector_per_block);  // number of sectors
  // hard drive geometry
  // - many stacked circular surfaces
  // - each surface has a head
  // - each surface has tracks (concentric circles)
  // - cylinder - track number on all surfaces
  // - sector number acts as a kind of address with each part specifying a different geometric component
  // - 8 bits drive and/or head plus flags, 16 bits cylinder, 7 bits sector
  outb(0x1f3, sector & 0xff); // sector number register
  outb(0x1f4, (sector >> 8) & 0xff); // cylinder low register
  outb(0x1f5, (sector >> 16) & 0xff); // cylinder high register
  outb(0x1f6, 0xe0 | ((b->dev&1)<<4) | ((sector>>24)&0x0f)); // drive/head register
  if(b->flags & B_DIRTY){
    outb(0x1f7, write_cmd);
    // write data from string, 4 bytes at a time
    outsl(0x1f0, b->data, BSIZE/4);
  } else {
    outb(0x1f7, read_cmd);
  }
}

// Interrupt handler when disk is done reading or writing
// trap() directs all disk interrupts here
void
ideintr(void)
{
  struct buf *b;

  // First queued buffer is the active request.
  // don't use a sleep lock because this is an interrupt handler function, so interrupts are disabled
  // requests are stored in the global idequeue linked list, interrupt usually means disk is done with the most recent request
  acquire(&idelock);

  if((b = idequeue) == 0){
    release(&idelock);
    return;
  }
  idequeue = b->qnext;

  // Read data if needed DIRTY flag set
  // using CPU instructions to move data to/from device hardware is called programmed I/O
  if(!(b->flags & B_DIRTY) && idewait(1) >= 0)
    insl(0x1f0, b->data, BSIZE/4);

  // Wake process sleeping on a channel for this buf.
  b->flags |= B_VALID;
  b->flags &= ~B_DIRTY;
  wakeup(b);

  // Start disk on next buf in queue.
  if(idequeue != 0)
    idestart(idequeue);

  release(&idelock);
}

//PAGEBREAK!
// Sync buf with disk.
// If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from disk, set B_VALID.
// mechanism for kernel and user threads to read/write disk data without calling static idestart()
// processes should never call this directly, it only gets called by the buffer cache code
// i.e. processes only use the universal I/O API
// when called process should be holding a sleep lock b->lock and either B_DIRTY set or B_VALID absent
// Simple IDE disk controller can only handle one operationa t a time
// Disk driver maintains the invariant that it has sent the buffer at the front of the queue to disk hardware
void
iderw(struct buf *b)
{
  struct buf **pp;

  if(!holdingsleep(&b->lock))
    panic("iderw: buf not locked");
  if((b->flags & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  if(b->dev != 0 && !havedisk1)
    panic("iderw: ide disk 1 not present");

  acquire(&idelock);  //DOC:acquire-lock

  // Append b to idequeue.
  // In the Style of Linux Torvalds
  // https://github.com/mkirchner/linked-list-good-taste
  b->qnext = 0;
  for(pp=&idequeue; *pp; pp=&(*pp)->qnext)  //DOC:insert-queue
    ;
  *pp = b;

  // if other buffers are in front, ideintr() means each disk interrupt start the disk on the next operation
  // otherwise, start the disk
  if(idequeue == b)
    idestart(b);

  // Now this process just has to wait for the request to finish.
  // wait until buffer has been synchronized with disk
  while((b->flags & (B_VALID|B_DIRTY)) != B_VALID){
    // will release and reactuire idelock before returning
    sleep(b, &idelock);
  }


  release(&idelock);
}

// Real world
// supporting all the devices on a PC motherboard is much work - there are many devices with many features
// and complex protocols
// drivers make up the majority of OS code
// typically devices are slower than the CPU, so interrupts are used
// modern disk controllers accept a batch of disk requests at a time and even reorder them (older OSes did
// this themselves)
// SSDs also provide block-based interfaces
// Other hardware is also surprisingly similar to disks
// - network device bufers hold packets
// - audio device buffers hold sound samples
// - graphics card buffers hold video data and command sequences
// High-bandwidth devices such as disks, graphics cards and network cards often use DMA instead of
// programmed I/O
// driver gives the device the physical address of the buffer's data and interrupt happens on copy
// faster and more efficient, and is less taxing for the CPU's memory caches
// Some drivers dynamically switch between polling and interrupts, because using interrupts can be
// expensive, but using polling can introduce delay until the driver processes an event
// e.g. a network driver that receives a burst of packets may switch from interrupts to polling, then switch
// back once all packets are processed
// Some drivers configure the IO APIC to route interrupts to multiple processors to load balance
// e.g. network driver arranges interrupts for packets of one network connection to the processor managing
// that connection
// can get quite sophisticated if connection durations vary and the OS wants to keep all processors busy
// to achieve high throughput
// Reading a file then sending over a network involves 4 copies - disk, kernel space, user space, kernel
// space, network device
// To support applications for which efficiency is important, OS use special code paths to avoid copies
// e.g. buffer cache block size typically matches the hardware page size, so read-only copies can be
// mapped into a process's address space using the paging hardware, without any copying
