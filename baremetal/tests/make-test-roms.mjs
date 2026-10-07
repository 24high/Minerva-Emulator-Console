#!/usr/bin/env node
// Writes tiny self-made test ROMs (no game code) for every bundled system.
// Most just loop with a valid header; the GBA ROM counts frames into the
// backdrop colour (see below). The save-* ROMs have battery-backed memory and
// add 1 to its first byte when they start, so two runs in a row must leave a
// save file that went up by one (arm-smoke.mjs --saves).
//
// usage: node baremetal/tests/make-test-roms.mjs <output dir>
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..");
const outDir = path.resolve(process.argv[2] || path.join(repoRoot, "baremetal", "build-node", "test-roms"));
fs.mkdirSync(outDir, { recursive: true });

function write(name, data) {
  fs.writeFileSync(path.join(outDir, name), data);
}

function toolchainBin() {
  const local = path.join(repoRoot, "toolchain");
  const dir = fs.readdirSync(local).find((entry) => entry.endsWith("arm-none-eabi"));
  if (!dir) throw new Error("arm-none-eabi toolchain not found in ./toolchain");
  return path.join(local, dir, "bin");
}

// NES: NROM, "JMP $8000", all vectors at $8000. The save ROM has the battery
// flag (8 KB at $6000) and runs "INC $6000" first.
function nes(save) {
  const prg = Buffer.alloc(32768);
  prg.set(save ? [0xee, 0x00, 0x60, 0x4c, 0x03, 0x80] : [0x4c, 0x00, 0x80], 0);
  for (let v = 0x7ffa; v < 0x8000; v += 2) prg.writeUInt16LE(0x8000, v);
  const flags6 = save ? 0x02 : 0x00;
  return Buffer.concat([Buffer.from([0x4e, 0x45, 0x53, 0x1a, 2, 1, flags6, 0]), Buffer.alloc(8), prg, Buffer.alloc(8192)]);
}
write("test.nes", nes(false));
write("save-nes.nes", nes(true));

// Game Boy / Color: "nop; jp $0150", "jr -2" at $0150, valid header checksum.
// The save ROM is MBC3 with clock, 8 KB RAM and battery (like Pokemon Gold):
// it enables the RAM and runs "inc ($a000)" first.
function gameBoy(color, save) {
  const rom = Buffer.alloc(32768);
  rom.set([0x00, 0xc3, 0x50, 0x01], 0x100);
  rom.write("SMOKETEST", 0x134, "latin1");
  if (color) rom[0x143] = 0x80;
  if (save) {
    rom[0x147] = 0x10; // MBC3+TIMER+RAM+BATTERY
    rom[0x149] = 0x02; // 8 KB RAM
  }
  let checksum = 0;
  for (let i = 0x134; i < 0x14d; i++) checksum = (checksum - rom[i] - 1) & 0xff;
  rom[0x14d] = checksum;
  rom.set(save
    ? [0x3e, 0x0a, 0xea, 0x00, 0x00, 0x21, 0x00, 0xa0, 0x34, 0x18, 0xfe] // ld a,$0a; ld ($0000),a; ld hl,$a000; inc (hl); jr $
    : [0x18, 0xfe], 0x150);
  return rom;
}
write("test.gb", gameBoy(false, false));
write("test.gbc", gameBoy(true, false));
write("save-gb.gb", gameBoy(false, true));

// SNES: 256 KB LoROM, "bra -2" at $00:8000, header at $7FC0. The save ROM
// declares 8 KB battery SRAM ($70:0000) and increments its first byte.
function snes(save) {
  const rom = Buffer.alloc(256 * 1024);
  rom.set(save
    ? [0xaf, 0x00, 0x00, 0x70, 0x1a, 0x8f, 0x00, 0x00, 0x70, 0x80, 0xfe] // lda $700000; inc; sta $700000; bra $
    : [0x80, 0xfe], 0);
  rom.write("SMOKE TEST           ", 0x7fc0, "latin1");
  rom.set([0x20, save ? 0x02 : 0x00, 0x08, save ? 0x03 : 0x00, 0x01, 0x33], 0x7fd5);
  rom.writeUInt16LE(0x8000, 0x7ffc);
  rom.writeUInt16LE(0x8000, 0x7fea);
  let sum = 0;
  for (const byte of rom) sum = (sum + byte) & 0xffff;
  rom.writeUInt16LE(sum ^ 0xffff, 0x7fdc);
  rom.writeUInt16LE(sum, 0x7fde);
  return rom;
}
write("test.sfc", snes(false));
write("save-sfc.sfc", snes(true));

// SNES that shows the pressed buttons as backdrop colour (CGRAM colour 0 =
// JOY1L | JOY1H << 8 from the automatic joypad read), for arm-smoke.mjs --input.
{
  const rom = snes(false);
  rom.set([
    0x78,                   // sei
    0xa9, 0x01, 0x8d, 0x00, 0x42, // lda #$01; sta $4200 (automatic joypad read)
    0xa9, 0x0f, 0x8d, 0x00, 0x21, // lda #$0f; sta $2100 (screen on)
    0xa9, 0x00, 0x8d, 0x21, 0x21, // loop: lda #$00; sta $2121 (CGRAM address 0)
    0xad, 0x18, 0x42, 0x8d, 0x22, 0x21, // lda $4218; sta $2122
    0xad, 0x19, 0x42, 0x29, 0x7f, 0x8d, 0x22, 0x21, // lda $4219; and #$7f; sta $2122
    0x80, 0xeb,             // bra loop (-21)
  ], 0);
  let sum = 0;
  rom.writeUInt16LE(0, 0x7fdc);
  rom.writeUInt16LE(0, 0x7fde);
  for (const byte of rom) sum = (sum + byte) & 0xffff;
  rom.writeUInt16LE(sum ^ 0xffff, 0x7fdc);
  rom.writeUInt16LE(sum, 0x7fde);
  write("input-snes.sfc", rom);
}

// Mega Drive: vectors, "SEGA MEGA DRIVE" header, "bra.s *" at $200. The save
// ROM declares battery SRAM on odd bytes at $200001-$203FFF ("RA" header) and
// runs "addq.b #1,$200001" first.
function megaDrive(save) {
  const rom = Buffer.alloc(128 * 1024);
  rom.writeUInt32BE(0x00fffe00, 0);
  for (let v = 1; v < 64; v++) rom.writeUInt32BE(0x200, v * 4);
  rom.write("SEGA MEGA DRIVE ", 0x100, "latin1");
  rom.write("SMOKE TEST      ", 0x150, "latin1");
  rom.writeUInt32BE(0, 0x1a0);
  rom.writeUInt32BE(rom.length - 1, 0x1a4);
  if (save) {
    rom.write("RA", 0x1b0, "latin1");
    rom.set([0xf8, 0x20], 0x1b2);
    rom.writeUInt32BE(0x200001, 0x1b4);
    rom.writeUInt32BE(0x203fff, 0x1b8);
    rom.set([0x52, 0x39, 0x00, 0x20, 0x00, 0x01, 0x60, 0xfe], 0x200);
  } else {
    rom.set([0x60, 0xfe], 0x200);
  }
  return rom;
}
write("test.md", megaDrive(false));
write("save-md.md", megaDrive(true));

// Master System / Game Gear: "di; jr -2", "TMR SEGA" header. The save ROMs
// switch on the cartridge RAM of the Sega mapper and run "inc ($8000)".
function sega8(region, save) {
  const rom = Buffer.alloc(32768);
  rom.set(save
    ? [0xf3, 0x31, 0xf0, 0xdf, 0x3e, 0x08, 0x32, 0xfc, 0xff, 0x21, 0x00, 0x80, 0x34, 0x18, 0xfe]
    : [0xf3, 0x18, 0xfe], 0);
  rom.write("TMR SEGA", 0x7ff0, "latin1");
  rom[0x7fff] = region;
  return rom;
}
write("test.sms", sega8(0x4c, false));
write("test.gg", sega8(0x6c, false));
write("save-sms.sms", sega8(0x4c, true));
write("save-gg.gg", sega8(0x6c, true));

// Game Boy Advance: like a real game, the main loop runs in Thumb code and
// waits for the VBlank interrupt through the BIOS (swi VBlankIntrWait); each
// frame it writes a counter as backdrop colour. After n frames the screen
// colour is n, so a black picture or a wrong colour shows a failure in the
// dynarec, the interrupt handling or the BIOS.
// The save ROM ("SRAM_V" marks a cartridge with SRAM) first increments the
// first SRAM byte at $0E000000.
function gba(save, input = false) {
  const bin = toolchainBin();
  const work = fs.mkdtempSync(path.join(os.tmpdir(), "gba-rom-"));
  const source = path.join(work, "rom.s");
  fs.writeFileSync(source, `
    .arm
    .global _start
_start:
    b       main
    .space  0xc0 - 4
main:
    ldr     r0, =0x03007ffc         @ user IRQ handler for the BIOS
    ldr     r1, =irq_handler
    str     r1, [r0]
    mov     r0, #0x04000000
    mov     r1, #0
    strh    r1, [r0]                @ DISPCNT: mode 0, no backgrounds
    mov     r1, #8
    strh    r1, [r0, #4]            @ DISPSTAT: VBlank IRQ
    add     r2, r0, #0x200
    mov     r1, #1
    strh    r1, [r2]                @ IE: VBlank
    strh    r1, [r2, #8]            @ IME
${save ? `
    mov     r0, #0x0e000000         @ SRAM
    ldrb    r1, [r0]
    add     r1, r1, #1
    strb    r1, [r0]` : ""}
    ldr     r3, =thumb_main + 1
    bx      r3

irq_handler:
    mov     r0, #0x04000000
    add     r0, r0, #0x200
    mov     r1, #1
    strh    r1, [r0, #2]            @ acknowledge in IF
    ldr     r0, =0x03007ff8         @ BIOS interrupt check flags
    ldrh    r1, [r0]
    orr     r1, r1, #1
    strh    r1, [r0]
    bx      lr
    .pool

    .thumb
    .thumb_func
thumb_main:
    mov     r4, #0
    ldr     r5, =0x05000000
    ldr     r6, =0x04000130         @ KEYINPUT
loop:
    swi     5                       @ VBlankIntrWait
${input ? `
    ldrh    r4, [r6]                @ pressed buttons (KEYINPUT is active low)
    mvn     r4, r4
    lsl     r4, r4, #22
    lsr     r4, r4, #22` : `
    add     r4, #1`}
    strh    r4, [r5]                @ backdrop colour = frame counter or buttons
    b       loop
    .pool
${save ? `    .align  2
    .ascii  "SRAM_V113"` : ""}
`);
  const object = path.join(work, "rom.o");
  const elf = path.join(work, "rom.elf");
  const raw = path.join(work, "rom.bin");
  for (const [tool, args] of [
    ["arm-none-eabi-as", ["-mcpu=arm7tdmi", "-o", object, source]],
    ["arm-none-eabi-ld", ["-Ttext=0x08000000", "-o", elf, object]],
    ["arm-none-eabi-objcopy", ["-O", "binary", elf, raw]],
  ]) {
    const result = spawnSync(path.join(bin, tool), args, { encoding: "utf8" });
    if (result.status !== 0) throw new Error(`${tool}: ${result.stderr}`);
  }

  const rom = Buffer.alloc(256 * 1024);
  fs.readFileSync(raw).copy(rom);
  rom.write("SMOKETEST\0\0\0", 0xa0, "latin1");
  rom.write("TEST01", 0xac, "latin1");
  rom[0xb2] = 0x96;
  let checksum = 0;
  for (let i = 0xa0; i < 0xbd; i++) checksum = (checksum - rom[i]) & 0xff;
  rom[0xbd] = (checksum - 0x19) & 0xff;
  fs.rmSync(work, { recursive: true, force: true });
  return rom;
}
write("test.gba", gba(false));
write("save-gba.gba", gba(true));
// Shows the pressed GBA buttons (KEYINPUT) as backdrop colour, for
// arm-smoke.mjs --input.
write("input-gba.gba", gba(false, true));


// --- Systems added later ------------------------------------------------------

// Atari 2600: a frame with VSYNC/VBLANK/192 lines/overscan, background colour
// $1E (yellow).
{
  const rom = Buffer.alloc(4096);
  rom.set([
    0x78, 0xd8, 0xa2, 0xff, 0x9a,             // sei; cld; ldx #$ff; txs
    0xa9, 0x02, 0x85, 0x00,                   // frame: lda #2; sta VSYNC
    0x85, 0x02, 0x85, 0x02, 0x85, 0x02,       // sta WSYNC x3
    0xa9, 0x00, 0x85, 0x00,                   // lda #0; sta VSYNC
    0xa9, 0x02, 0x85, 0x01,                   // lda #2; sta VBLANK
    0xa2, 0x25, 0x85, 0x02, 0xca, 0xd0, 0xfb, // ldx #37; vb: sta WSYNC; dex; bne vb
    0xa9, 0x00, 0x85, 0x01,                   // lda #0; sta VBLANK
    0xa9, 0x1e, 0x85, 0x09,                   // lda #$1e; sta COLUBK
    0xa2, 0xc0, 0x85, 0x02, 0xca, 0xd0, 0xfb, // ldx #192; vis: sta WSYNC; dex; bne vis
    0xa9, 0x02, 0x85, 0x01,                   // lda #2; sta VBLANK
    0xa2, 0x1e, 0x85, 0x02, 0xca, 0xd0, 0xfb, // ldx #30; os: sta WSYNC; dex; bne os
    0x4c, 0x05, 0xf0,                         // jmp frame
  ], 0);
  rom.writeUInt16LE(0xf000, 0xffc);
  rom.writeUInt16LE(0xf000, 0xffe);
  write("test.a26", rom);
}

// Atari Lynx: homebrew file (BS93) loaded to $0200; sets up the video timers
// and the display like the boot ROM does, with colour 0 red.
{
  const code = [
    0x78, 0xd8,                               // sei; cld
    0xa9, 158, 0x8d, 0x00, 0xfd,              // timer 0 (lines): backup 158
    0xa9, 0x18, 0x8d, 0x01, 0xfd,             //   control: reload, count
    0xa9, 104, 0x8d, 0x08, 0xfd,              // timer 2 (frames): backup 104
    0xa9, 0x1f, 0x8d, 0x09, 0xfd,             //   control: linked to timer 0
    0xa9, 0x00, 0x8d, 0x94, 0xfd,             // DISPADR = $c000
    0xa9, 0xc0, 0x8d, 0x95, 0xfd,
    0xa9, 0x29, 0x8d, 0x93, 0xfd,             // PBKUP
    0xa9, 0x0d, 0x8d, 0x92, 0xfd,             // DISPCTL: display on
    0xa9, 0x00, 0x8d, 0xa0, 0xfd,             // GREEN0 = 0
    0xa9, 0x0f, 0x8d, 0xb0, 0xfd,             // BLUERED0 = red
    0x80, 0xfe,                               // bra *
  ];
  const header = Buffer.from([0x80, 0x08, 0x02, 0x00, 0, 0, 0x42, 0x53, 0x39, 0x33]);
  header.writeUInt16BE(code.length + header.length, 4);
  write("test.lnx", Buffer.concat([header, Buffer.from(code)]));
}

// PC Engine: 8 KB HuCard, code at $e000; VCE colours 0 and $100 (shown
// while the display is off) red.
{
  const rom = Buffer.alloc(8192);
  rom.set([
    0x78, 0xd4, 0xd8,                         // sei; csh; cld
    0xa9, 0xff, 0x53, 0x01,                   // lda #$ff; tam #0 (I/O)
    0xa9, 0xf8, 0x53, 0x02,                   // lda #$f8; tam #1 (RAM)
    0xa2, 0xff, 0x9a,                         // ldx #$ff; txs
    0x9c, 0x02, 0x04, 0x9c, 0x03, 0x04,       // stz $0402; stz $0403 (colour 0)
    0xa9, 0x38, 0x8d, 0x04, 0x04,             // lda #$38; sta $0404 (red)
    0x9c, 0x05, 0x04,                         // stz $0405
    0x9c, 0x02, 0x04, 0xa9, 0x01, 0x8d, 0x03, 0x04, // stz $0402; lda #1; sta $0403 (colour $100)
    0xa9, 0x38, 0x8d, 0x04, 0x04,             // lda #$38; sta $0404 (red)
    0x9c, 0x05, 0x04,                         // stz $0405
    0x80, 0xfe,                               // bra *
  ], 0);
  rom.writeUInt16LE(0xe000, 0x1ffe);
  write("test.pce", rom);
}

// WonderSwan: 1 MB ROM; the reset vector (last 16 bytes) jumps to F000:0000,
// which loops.
{
  const rom = Buffer.alloc(1024 * 1024, 0xff);
  const base = rom.length - 0x10000;
  rom.set([0xfa, 0xeb, 0xfe], base);         // cli; jmp $
  rom.set([0xea, 0x00, 0x00, 0x00, 0xf0], rom.length - 16); // jmp far F000:0000
  rom.set([0x00, 0x00, 0x01, 0x00, 0x06, 0x00, 0x04, 0x00], rom.length - 10);
  let sum = 0;
  for (let i = 0; i < rom.length - 2; i++) sum = (sum + rom[i]) & 0xffff;
  rom.writeUInt16LE(sum, rom.length - 2);
  write("test.ws", rom);
}

// ZX Spectrum: 48K snapshot (.sna); the program sets a red border and loops.
{
  const header = Buffer.alloc(27);
  header.writeUInt16LE(0xff00, 23);          // SP: the PC is popped from there
  header[25] = 1;                             // IM 1
  header[26] = 2;                             // border red
  const ram = Buffer.alloc(49152);
  ram.set([0xf3, 0x3e, 0x02, 0xd3, 0xfe, 0x18, 0xfe], 0x8000 - 0x4000); // di; ld a,2; out ($fe),a; jr $
  ram.writeUInt16LE(0x8000, 0xff00 - 0x4000);
  write("test.sna", Buffer.concat([header, ram]));
}

// ZX Spectrum tape (.tap): BASIC program with autostart, loaded by the
// core's auto load ("LOAD """); red border, then the Kempston port and the
// key pressed in the top left corner (the pad is a cursor joystick, keys 5
// to 8 and 0, and a Kempston joystick):
//   10 BORDER 2 / 20 PRINT AT 0,0;IN 31;" ";INKEY$;"   " / 30 GOTO 20
{
  const smallInt = (value) => [0x0e, 0x00, 0x00, value & 0xff, value >> 8, 0x00];
  const number = (value) => [...Buffer.from(String(value), "latin1"), ...smallInt(value)];
  const text = (value) => [0x22, ...Buffer.from(value, "latin1"), 0x22];
  const line = (lineNumber, tokens) => {
    const body = [...tokens, 0x0d];
    return [lineNumber >> 8, lineNumber & 0xff, body.length & 0xff, body.length >> 8, ...body];
  };
  const program = Buffer.from([
    ...line(10, [0xe7, ...number(2)]),                                        // BORDER 2
    ...line(20, [0xf5, 0xac, ...number(0), 0x2c, ...number(0), 0x3b,          // PRINT AT 0,0;
      0xbf, ...number(31), 0x3b, ...text(" "), 0x3b, 0xa6, 0x3b, ...text("   ")]), // IN 31;" ";INKEY$;"   "
    ...line(30, [0xec, ...number(20)]),                                       // GOTO 20
  ]);
  const block = (flag, data) => {
    let checksum = flag;
    for (const byte of data) checksum ^= byte;
    const out = Buffer.alloc(data.length + 4);
    out.writeUInt16LE(data.length + 2, 0);
    out[2] = flag;
    data.copy(out, 3);
    out[out.length - 1] = checksum;
    return out;
  };
  const header = Buffer.alloc(17);
  header[0] = 0;                              // program
  header.write("TEST      ", 1, "latin1");
  header.writeUInt16LE(program.length, 11);
  header.writeUInt16LE(10, 13);               // autostart line
  header.writeUInt16LE(program.length, 15);   // start of the variables
  write("test.tap", Buffer.concat([block(0x00, header), block(0xff, program)]));
}

// ZX81: a .p file (memory from the system variables at $4009 on) with a
// program saved without autostart (NXTLIN at the display file), which the
// core starts anyway (patches/81-libretro). It fills the screen with inverse
// spaces: the screen turns black once it runs.
//   10 FOR I=1 TO 704 / 20 PRINT CHR$ 128; / 30 NEXT I / 40 GOTO 40
{
  const digits = (text) => [...text].map((ch) => 0x1c + Number(ch));       // ZX81 '0'..'9'
  const number = (text, float) => [...digits(text), 0x7e, ...float];      // digits + hidden float
  const line = (lineNumber, tokens) => {
    const body = [...tokens, 0x76];                                          // NEWLINE
    return [lineNumber >> 8, lineNumber & 0xff, body.length & 0xff, body.length >> 8, ...body];
  };
  const I = 0x2e;
  const program = [
    ...line(10, [0xeb, I, 0x14, ...number("1", [0x81, 0, 0, 0, 0]), 0xdf, ...number("704", [0x8a, 0x30, 0, 0, 0])]), // FOR I=1 TO 704
    ...line(20, [0xf5, 0xd6, ...number("128", [0x88, 0, 0, 0, 0]), 0x19]),  // PRINT CHR$ 128;
    ...line(30, [0xf3, I]),                                                 // NEXT I
    ...line(40, [0xec, ...number("40", [0x86, 0x20, 0, 0, 0])]),            // GOTO 40
  ];
  const base = 0x4009;
  const programStart = 0x407d;
  const dFile = programStart + program.length;
  const vars = dFile + 25;                    // collapsed display: 25 x HALT
  const eLine = vars + 1;                     // after the $80 end marker
  const memory = Buffer.alloc(eLine - base + 3);
  const put16 = (address, value) => memory.writeUInt16LE(value, address - base);
  put16(0x400c, dFile);                       // D_FILE
  put16(0x400e, dFile + 1);                   // DF_CC
  put16(0x4010, vars);                        // VARS
  put16(0x4014, eLine);                       // E_LINE
  put16(0x4016, eLine + 1);                   // CH_ADD
  put16(0x401a, eLine + 3);                   // STKBOT
  put16(0x401c, eLine + 3);                   // STKEND
  put16(0x401f, 0x405d);                      // MEM = MEMBOT
  memory[0x4022 - base] = 2;                  // DF_SZ
  put16(0x4025, 0xffff);                      // LAST_K
  memory[0x4028 - base] = 55;                 // MARGIN (PAL)
  put16(0x4029, dFile);                       // NXTLIN: no autostart, stop after loading
  put16(0x4034, 0xffff);                      // FRAMES
  memory[0x4038 - base] = 0xbc;               // PR_CC
  put16(0x4039, 0x1821);                      // S_POSN
  memory[0x403b - base] = 0x40;               // CDFLAG: SLOW mode
  memory[0x405c - base] = 0x76;               // end of the printer buffer
  memory.set(program, programStart - base);
  memory.fill(0x76, dFile - base, vars - base);
  memory[vars - base] = 0x80;                 // end of the variables
  memory[eLine - base] = 0x76;                // empty edit line
  memory[eLine + 1 - base] = 0x80;
  write("test.p", memory);
}

// Amstrad CPC: data format disk (40 tracks, 9 sectors of 512 bytes) with an
// ASCII BASIC program that the core starts (RUN"JOY) and that shows JOY(0)
// in the top left corner: 1 up, 2 down, 4 left, 8 right, 16 fire (pad A),
// 32 second fire button (pad B).
{
  const program = Buffer.from("10 LOCATE 1,1:PRINT JOY(0);\"  \"\r\n20 GOTO 10\r\n\x1a", "latin1");
  const header = Buffer.alloc(256);
  header.write("MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 0, "latin1");
  header.write("Minerva", 0x22, "latin1");
  header[0x30] = 40;
  header[0x31] = 1;
  header.writeUInt16LE(256 + 9 * 512, 0x32);
  const tracks = [];
  for (let t = 0; t < 40; t++) {
    const info = Buffer.alloc(256);
    info.write("Track-Info\r\n", 0, "latin1");
    info[0x10] = t;
    info[0x14] = 2;
    info[0x15] = 9;
    info[0x16] = 0x4e;
    info[0x17] = 0xe5;
    for (let s = 0; s < 9; s++) info.set([t, 0, 0xc1 + s, 2, 0, 0, 0, 0], 0x18 + s * 8);
    tracks.push(info, Buffer.alloc(9 * 512, 0xe5));
  }
  // Directory in block 0 (track 0, sectors &C1-&C2), the program in block 2
  // (sectors &C5-&C6).
  const entry = Buffer.alloc(32, 0);
  entry.write("JOY     BAS", 1, "latin1");
  entry[15] = Math.ceil(program.length / 128);
  entry[16] = 2;
  tracks[1].set(entry, 0);
  tracks[1].set(program, 4 * 512);
  write("test.dsk", Buffer.concat([header, ...tracks]));
}

// C64: blank formatted disk (BAM and empty directory on track 18); the C64
// starts its BASIC.
{
  const sectorsOf = (track) => (track <= 17 ? 21 : track <= 24 ? 19 : track <= 30 ? 18 : 17);
  const offset = (track, sector) => {
    let blocks = 0;
    for (let t = 1; t < track; t++) blocks += sectorsOf(t);
    return (blocks + sector) * 256;
  };
  const disk = Buffer.alloc(174848);
  const bam = offset(18, 0);
  disk.set([18, 1, 0x41, 0], bam);
  for (let t = 1; t <= 35; t++) {
    const free = t === 18 ? sectorsOf(t) - 2 : sectorsOf(t);
    const bits = (2 ** sectorsOf(t) - 1) & ~(t === 18 ? 3 : 0);
    disk.set([free, bits & 0xff, (bits >> 8) & 0xff, (bits >> 16) & 0xff], bam + 4 * t);
  }
  disk.fill(0xa0, bam + 0x90, bam + 0xab);
  disk.write("MINERVA", bam + 0x90, "latin1");
  disk.write("01", bam + 0xa2, "latin1");
  disk.write("2A", bam + 0xa5, "latin1");
  disk.set([0, 0xff], offset(18, 1));
  write("test.d64", disk);
}

// PICO-8: text cart that clears the screen to colour 8 (red).
write("test.p8", "pico-8 cartridge // http://www.pico-8.com\nversion 41\n__lua__\nfunction _draw()\n cls(8)\nend\n");

// MAME: Space Invaders ROM set with empty ROMs (right names and sizes, wrong
// checksums): checks loading a zipped ROM set and running the driver.
{
  const crcTable = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc32 = (data) => {
    let c = 0xffffffff;
    for (const byte of data) c = crcTable[(c ^ byte) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const files = ["invaders.h", "invaders.g", "invaders.f", "invaders.e"].map((name) => ({ name, data: Buffer.alloc(2048) }));
  const parts = [];
  const central = [];
  let position = 0;
  for (const file of files) {
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(10, 4);
    local.writeUInt32LE(crc32(file.data), 14);
    local.writeUInt32LE(file.data.length, 18);
    local.writeUInt32LE(file.data.length, 22);
    local.writeUInt16LE(file.name.length, 26);
    const entry = Buffer.alloc(46);
    entry.writeUInt32LE(0x02014b50, 0);
    entry.writeUInt16LE(20, 4);
    entry.writeUInt16LE(10, 6);
    entry.writeUInt32LE(crc32(file.data), 16);
    entry.writeUInt32LE(file.data.length, 20);
    entry.writeUInt32LE(file.data.length, 24);
    entry.writeUInt16LE(file.name.length, 28);
    entry.writeUInt32LE(position, 42);
    parts.push(local, Buffer.from(file.name, "latin1"), file.data);
    central.push(entry, Buffer.from(file.name, "latin1"));
    position += 30 + file.name.length + file.data.length;
  }
  const directory = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(files.length, 8);
  end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(position, 16);
  write("invaders.zip", Buffer.concat([...parts, directory, end]));
}

console.log(`Test ROMs in ${outDir}`);
