#include "types.h"
#include "x86.h"
#include "defs.h"
#include "kbd.h"

// the keyboard driver is generally not responsible for translating scancodes into printable characters
// the driver's purpose is to deal with device specifics and provide a generic interface for getting key
// presses/releases
// however it does usually involve translating from the keyboard-specific scancode into an OS-specific one
// the idea is that if more scancode sets or keyboards (like USB) are supported later on, these can
// be added without having to modify any code that uses the keyboard
// simply write a new driver that provides the same interface

// the driver does care about keeping track of events
// quite often these events are consumed on read and removed from the buffer

// problems to solve
// - implement the driver in a generic fashion to make adding other scansets easier in the future
// - store the history of key presses and their statuses somewhere
// - handle special keys and modifiers
// - handle press/release status if needed
// - try not to lose sequence of key pressed/released
// - handle the caps, num and scroll lock keys (with the LEDs), on PS/2 keyboards LEDs are controlled manually
// - optionally translate the scancode into a human-readable character when needed
// some of these are at a higher level and will be implemented "using" the driver, not by it

// circular buffer is simple and fast
// when full can drop oldest or latest scancodes
// interrupt handler simply appends to buffer
// #define MAX_KEYB_BUFFER_SIZE 255
// typedef struct {
//     uchar code;
//     uchar status_mask; // bitfield for shift, etc. so applications don't have to keep track of modifier
//                        // state themselves
// } key_event; // extensible
// key_event keyboard_buffer[MAX_KEYB_BUFFER_SIZE];
// uchar buf_position = 0;

// to keep track of multi-byte, multi-interrupt scancodes, we can implement a state machine
// #define NORMAL_STATE 0
// #define PREFIX_STATE 1
// state machine implementation breaks down for 4-byte scancodes, as you need a separate state for each seq
// alternative implementation - append scancodes to a buffer until a full one is received, then reset
// in init() initiate state to NORMAL_STATE
// react appropriately based on state
// - don't need to store 0xE0 bytes
// - change state at the end appropriately

// keyboard-specific scancode to kernel scancode translation
// arbitrary kernel-specific scancodes
// could use '0' to mean 'keyboard scancode not supported/no translation'
// typedef enum kernel_scancodes {
//     ...
//     F1 = 0xAABBCCDD,
//     ...
// };
// kernel_scancodes scancode_mapping[] = {
//     ...
//     F1, (at the correct position)
//     ...
// };

// kernel scancode to printable character translation
// could have 2 lookup tables for shifted and non-shifted keys
// could have 1 lookup table and an offset to calculate shifted index value, but this doesn't work for non-us
// keyboards or symbols (not expandable)
// could use a big switch statement with if/elses to handle shifting

int
kbdgetc(void)
{
  static uint shift;
  static uchar *charcode[4] = {
    normalmap, shiftmap, ctlmap, ctlmap
  };
  uint st, data, c;

  st = inb(KBSTATP);
  if((st & KBS_DIB) == 0)
    return -1;
  data = inb(KBDATAP);

  if(data == 0xE0){
    // first part of multi-byte scancode (extended key)
    shift |= E0ESC;
    return 0;
  } else if(data & 0x80){
    // Key released
    data = (shift & E0ESC ? data : data & 0x7F); // mask out the 8th bit if not an E0 escape
    shift &= ~(shiftcode[data] | E0ESC); // clear released key and E0 escape
    return 0;
  } else if(shift & E0ESC){
    // Last character was an E0 escape (extended key)
    // OR with 0x80 to differentiate
    data |= 0x80;
    shift &= ~E0ESC; // clear E0 escape
  }

  shift |= shiftcode[data]; // set shift if a state key is pressed
  shift ^= togglecode[data]; // toggle shift if a toggle key is pressed
  c = charcode[shift & (CTL | SHIFT)][data]; // look up key in appropriate map
  if(shift & CAPSLOCK){ // adjust if capslock toggle
    if('a' <= c && c <= 'z')
      c += 'A' - 'a';
    else if('A' <= c && c <= 'Z')
      c += 'a' - 'A';
  }
  return c;
}

void
kbdintr(void)
{
  consoleintr(kbdgetc);
}
