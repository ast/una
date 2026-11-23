# PAH8316LS: what its registers were seen to do

PixArt publishes no datasheet for this part. Everything here was learned by
running PixArt's own library on the watch with its bus calls written down
(`zephyr/una/app/src/hr_ref.c`, `hr-ref.conf`) and reading the traces: the
library's start on the desk, and seven and a half minutes on a wrist with the
watch taken off and put on four times (441 batches, no bus errors). Nothing
here is from the library's code; where its disassembly is consulted later,
the item will say so.

*Seen* means on the bus. *Inferred* means our reading of what was seen, and
may be wrong.

## Bus

I2C at 400 kHz, address 0x40. Registers are a byte; a read or write of
several bytes runs on through the following registers, except at 0x7B (the
FIFO), which stays put. *Seen.*

Register 0x7F selects a bank, 0 to 5, in every bank. The library writes it
before nearly every access and does not rely on what it wrote last. *Seen.*

| Bank | What is in it (inferred) |
|---|---|
| 0 | Identity, run, interrupt flags, touch, FIFO, timing |
| 1 | Analogue front end, per-slot gains |
| 2 | Read-only measurements (exposure statistics) |
| 3 | Reset, clocks, power-down |
| 4 | LED drive per slot |
| 5 | Per-slot sequencing |

## Identity

Bank 0, 0x00 and 0x01 read `11 83`: product 0x8311, low byte first. The stock
firmware's factory test expects the same of a PAH8316. *Seen.*

## Supplies and reset

Sensor supply (PF13) and LED supply (PG10) on, 10 ms, then the wake below.
The chip answers at once after it. Off is the LED supply, then the sensor's,
and 20 ms before on again. *Seen; the times are the stock firmware's.*

The wake, written before the identity is read, before every setup, and
before power-down, all in bank 3:

    0B=36  54=14  54=10  0B=37  70=10  (1 ms)  70=02  (1 ms)
    01=01  (3 ms)  01=00  (15 ms)

0x0B looks like a write lock (36 opens, 37 closes: the other bank 3 writes
of a setup are bracketed the same way), 0x01=1 like power-down or reset and
0x01=0 its release. *Inferred.* Stopping is bank 1 `00=98`, then the wake as
far as `01=01` and its 3 ms, and nothing after: the chip is left in that
state and the supplies are cut. *Seen.*

## Setup

The library never changes a running configuration. Whenever it wants another
mode, it wakes the chip as above and writes everything again:

1. Reads bank 4 at 00 03 06 09 0C 0F 12 16 19 1C 1F 22 25 28, a byte each
   (all 0x96 after the wake), and does nothing visible with them.
2. The common part, some 230 single-byte writes over banks 5, 4, 3, 2, 1, 0
   in that order, the same every time.
3. A tail for the mode: *touch only* (4 writes) or *heart rate* (45 writes
   and some LED settings in bank 4).
4. Bank 0 `7E=80`, 1 ms, `07=01`: taken to be "apply" and "run". *Inferred.*

It does this at start (touch only), when skin is found (heart rate), and when
skin is lost (touch only again). With "always run" asked for, it goes to
heart rate at once. The whole of it takes about 55 ms. *Seen.*

The tables are in the driver (`zephyr/una/drivers/sensor/pah8316/`), as the
values seen on the bus; what the single values mean is mostly not known.
Those that can be read:

| Bank:reg | Touch only | Heart rate | Taken to be |
|---|---|---|---|
| 0:35, 0:36 | 0x0014 | 0x0028 | FIFO watermark, in entries |
| 0:03, 0:04 | 80, 0C | 40, 06 | Frame period (halved for heart rate) |
| 0:4F | 04 | 09 | ? |
| 0:1B | 10 | 11 | PPG off / on |
| 4:01 04 07, 4:17 1A 1D | 00 | 24 12 09 | LED currents of the green slots |
| 1:35..4C, 1:69..72 | — | set | Front-end gains for the green slots |

## Interrupt

The line (PF12) rises and stays up until the flags are cleared. *Seen.* Each
time, the library does:

    bank 0: read 2F (2 bytes), read 30 (1), read 38 (2)
    ... the FIFO, if its flag is up (below) ...
    bank 0: read 2F (2), write 2F=00, write 30=<what 30 read>

The second byte of 0x2F and the byte at 0x30 were always equal: the flags.
Writing them back to 0x30 clears them.

| Flags | When |
|---|---|
| 00 | Right after a setup; nothing to do |
| 14 | Skin found (in touch-only mode) |
| 01 | Skin lost |
| 18 | A batch is ready, skin there |
| 08 | A batch is ready, no skin ("always run") |

So bit 3 is the FIFO at its watermark, bit 4 skin present, bit 0 skin lost,
and bit 2 came only with the finding of skin. *Inferred.*

0x38, 16 bits low byte first, is the touch reading: below 0x0160 in free air
on the desk, 0x6400 or so on the wrist (0x9062 at most). The chip decides touch by
itself; the library only reads the figure. *Seen.*

## FIFO

    bank 0: read 44 (2 bytes): entries waiting, 0x0028 at the watermark
            write 7A=01
            read 7B, 5 bytes an entry, all of them in one transfer
            write 7A=00

An entry is five bytes:

| Byte | |
|---|---|
| 0 | The LED setting the sample was taken with, 2 to 255 |
| 1 | The channel: 0x08 or 0x18 |
| 2..4 | The sample, 24 bits, low byte first |

In the heart-rate setup there are two channels, alternating 08, 18, at 20 Hz
each, so 40 entries and one interrupt a second. *Seen.* Which photodiodes or
LEDs make a channel is not known; both carry the pulse.

**The chip sets its own LED.** In free air the LED byte is 255. On the wrist
it came down 255, 146, 83, 47, 26, 14, 7, 5, 4 from one sample to the next,
and was 8 for 96 in 100 of the samples after, with no write from the library
in between. So exposure control is the
chip's, and a driver need not do it. *Seen.*

What the library hands its algorithm is `sample * 256 / LED`: 565609 at 255
gives 567827, 397273 at 8 gives 12712736. That takes the LED's steps out of
the signal. *Seen, to the last digit.*

After each batch the library also reads 8 bytes each at bank 2, 50 58 60 68
32 3A. Only the first eight were ever other than zero; they look like four
16-bit figures, two a channel. The driver is to be tried without them.

## Not known

- What bits 0 to 2 of the flags are, exactly, and whether a FIFO overflow has
  a flag.
- The meaning of nearly all of the common setup.
- The 25 Hz profiles (red and infrared for SpO2, single green for HRV): not
  traced yet.
- What the chip draws in touch-only mode and in heart-rate mode.
