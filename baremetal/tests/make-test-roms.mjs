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


console.log(`Test ROMs in ${outDir}`);
