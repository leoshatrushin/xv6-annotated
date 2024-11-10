// Disk driver - a simple driver isn't enough
// - files don't really exist on disk, but the OS provides a file system abstraction, a simplified framework
// - also need to make sure concurrent accesses don't corrupt a file or file system
// - need to separate kernel data from user data
// - present EVERYTHING in the elegant abstraction of a file

// File system organization (layers)
// - disk driver - reads/writes blocks on an IDE hard drive
// - buffer cache - caches disk blocks in memory and synchronizes access to them
// - logging - provides atomic disk writes to mitigate the risk of a crash
// - inodes - turns disk blocks into individual files the OS can manipulate
// - directories - creates a tree of named directories that contain other files
// - path names - provides hierarchical, human-readable path names in the directory tree structure
// - file descriptors - resources abstracted by the OS to provide a unified API

// Hard drives are usually physically divided into sectors, traditionally of 512 bytes
// OS can collect these into larger blocks, which are multiples of the sector size
// xv6 uses 512-byte blocks for simplicity
// block 0 usually contains the boot sector, so it's not used by xv6
// xv6 actually stores the boot loader and kernel code on an entirely separate physical disk
// block 1 is called the superblock - it contains metadata about the file system (total size, log size, number of files, their locations)
// log starts at block 2

// interacting directly with the hardware means all kinds of opaque code with seemingly arbitrary port I/O and cryptic magic numbers, specific to the hardware

// represents a block
struct buf {
  int flags;
  uint dev; // device number
  uint blockno;
  struct sleeplock lock; // protect buffer
  uint refcnt; // processes using this buffer
  struct buf *prev; // buffer LRU cache doubly-linked list of buffers
  struct buf *next;
  struct buf *qnext; // disk driver singly-linked queue of buffers waiting to be read/written
  uchar data[BSIZE];
};
#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk

