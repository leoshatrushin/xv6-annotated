// Intel 8250 serial port (UART).

#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"

// alternative - 'debugcon'/'port e9 hack'
// special port that sends data directly to the emulator console output
// does not require any initialization

#define COM1    0x3f8

static int uart;    // is there a uart?

void
uartinit(void)
{
  char *p;
  
  // outb(COM1+1, 0x00);  // Disable all interrupts

  // Turn off the FIFO
  outb(COM1+2, 0);
  // outb(COM1+2,0xC7);    // Enable FIFO, clear them, with 14-byte threshold

  // 9600 baud, 8 data bits, 1 stop bit, parity off.

  // Set DLAB (Divisor Latch Access Bit); enable divisor
  // When set, +0 and +1 map to low and high bytes of Divisor register for setting the baud rate
  outb(COM1+3, 0x80);
  outb(COM1+0, 115200/9600); // Set divisor/set baud rate to 9600 (lo byte)
  outb(COM1+1, 0); // (hi byte)
  outb(COM1+3, 0x03);    // Lock divisor, break disable, 1 stop bit, no parity, 8 data bits
  outb(COM1+4, 0); // ?
  outb(COM1+1, 0x01);    // Enable receive interrupts (IRQ_COM1=4), maybe 0x0B for RTS/DSR set?
  // try removing the test if things aren't working
  // outb(COM1+4, 0x1E);    // Set in loopback mode, test the serial chip
  // oub(COM1+0, 0xAE);    // Send a test byte
  // if(inb(COM1+0) != 0xAE) return;
  // outb(COM1+4, 0x0F);    // Set in normal operation mode
                            // not loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled

  // If status is 0xFF, no serial port.
  if(inb(COM1+5) == 0xFF)
    return;
  uart = 1;

  // Acknowledge pre-existing interrupt conditions;
  // enable interrupts.
  inb(COM1+2);
  inb(COM1+0);
  ioapicenable(IRQ_COM1, 0);

  // Announce that we're here.
  for(p="xv6...\n"; *p; p++)
    uartputc(*p);
}

void
uartputc(int c)
{
  int i;

  if(!uart)
    return;
  for(i = 0; i < 128 && !(inb(COM1+5) & 0x20); i++)
    microdelay(10);
  outb(COM1+0, c);
}

static int
uartgetc(void)
{
  if(!uart)
    return -1;
  if(!(inb(COM1+5) & 0x01))
    return -1;
  return inb(COM1+0);
}

void
uartintr(void)
{
  consoleintr(uartgetc);
}
