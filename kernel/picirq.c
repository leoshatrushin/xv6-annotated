#include "types.h"
#include "x86.h"
#include "traps.h"

// I/O Ports of the two programmable interrupt controllers
#define IO_PIC1         0x20    // Master (IRQs 0-7)
#define IO_PIC2         0xA0    // Slave (IRQs 8-15)

// The old x86 architecture had two PIC processors, "master" and "slave"
// Master PIC command port 0x20, data port 0x21
// Slave PIC command port 0xA0, data port 0xA1
// ICW (initialization command words) values
// - ICW_1 (0x11) - start of initialization sequence
// - ICW_2 (0x20 master, 0x28 slave) - interrupt vector address value (IDT entries), i.e. offset IRQs to
//   not overlap with reserved vectors 0-31
// - ICW_3 (master 0x2, slave 0x4)
//   - used to indacte if the pin has a slave or not
//   - the slave PIC will be connected to one of the master interrupt pins and we need to indicate which
//   - in the case of a slave device the value will be its id
//   - on x86 the master irq pin connected to the slave is 2
// - ICW_4 - configuration bits for mode of operation (tell it to use 8086 mode)

// Don't use the 8259A interrupt controllers.  Xv6 assumes SMP hardware.
// CPU starts in PIC8259A emulation mode - LAPIC and I/O APIC emulate the old PIC
void
picinit(void)
{
  /*outportb(PIC_COMMAND_MASTER, ICW_1);*/
  /*outportb(PIC_COMMAND_SLAVE, ICW_1);*/
  /*outportb(PIC_DATA_MASTER, ICW_2_M);*/
  /*outportb(PIC_DATA_SLAVE, ICW_2_S);*/
  /*outportb(PIC_DATA_MASTER, ICW_3_M);*/
  /*outportb(PIC_DATA_SLAVE, ICW_3_S);*/
  /*outportb(PIC_DATA_MASTER, ICW_4);*/
  /*outportb(PIC_DATA_SLAVE, ICW_4);*/
  /*outportb(PIC_DATA_MASTER, 0xFF);*/
  /*outportb(PIC_DATA_SLAVE, 0xFF);*/
  // mask all interrupts
  outb(IO_PIC1+1, 0xFF);
  outb(IO_PIC2+1, 0xFF);
}

//PAGEBREAK!
// Blank page.
