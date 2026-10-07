#!/usr/bin/env node
// Runs the bundled cores as built for the kernel (baremetal/build-node/gpi-all/
// cores/*.o, including gpSP's ARM dynarec) on an emulated ARM1176 under
// qemu-arm, as an ARM Linux program driven by smoke_main.cpp.
// --board=pi5 does the same for the Pi 5 kernel (build-node/all, AArch64) on
// an emulated Cortex-A76 under qemu-aarch64, --board=gpi2 for the CM4 kernel
// of the GPi Case 2 (build-node/gpi2-all) on a Cortex-A72.
//
// Needs: a finished "npm run build:gpi" (or build:rpi5), qemu-arm or
// qemu-aarch64 (also as -static) and the test ROMs from make-test-roms.mjs.
//
// --fatfs links the real baremetal/libc layer and Circle's FatFs instead of
// Linux file access, reading the ROMs from a FAT32 image like on the SD card.
// --options=<file> puts that file into the image as minerva.cfg (core options).
// --sequence also runs all ROMs twice in one process with the same runner,
// as when games are ended with Start+Select and others started, and checks
// that every game gives exactly the result of its own run (cores restart
// cleanly after retro_deinit).
// --saves runs the save-* ROMs (battery saves, see make-test-roms.mjs) twice,
// in separate processes like across a reboot, and checks that the second run
// loaded the save of the first: exactly one byte went up by one. With --fatfs
// the saves go through platform/circle/circle_fs.cpp into the FAT image.
//
// --input runs the input-* ROMs, which show the buttons the emulated console
// sees as backdrop colour, with pressed pad buttons (SMOKE_BUTTONS, see
// smoke_main.cpp) and checks the GPi button layouts end to end: GBA Y/X as
// L/R, SNES Select+Y/X as L/R, Select held back, tapped and passed through.
//
// usage: node baremetal/tests/arm-smoke.mjs [--board=pi5|gpi2] [--fatfs [--options=<file>]] [--sequence | --saves | --input] [rom ...]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { buildImage } from "../sdcard.mjs";

const testsDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(testsDir, "..");
// --board=pi5 / --board=gpi2 (CM4): the AArch64 kernels.
const boardArg = process.argv.find((arg) => arg.startsWith("--board="));
const AARCH64_BOARDS = {
  "--board=pi5": { buildDir: "all", outDir: "aarch64-smoke", cpu: "cortex-a76" },
  "--board=gpi2": { buildDir: "gpi2-all", outDir: "aarch64-smoke-gpi2", cpu: "cortex-a72" },
};
const aarch64Board = AARCH64_BOARDS[boardArg];
const aarch64 = Boolean(aarch64Board);
const buildDir = path.join(projectRoot, "build-node", aarch64 ? aarch64Board.buildDir : "gpi-all");
const outDir = path.join(projectRoot, "build-node", aarch64 ? aarch64Board.outDir : "arm-smoke");
const qemuCpu = aarch64 ? aarch64Board.cpu : "arm1176";
const romDir = path.join(projectRoot, "build-node", "test-roms");
// Every isolated core of the kernel build (cores/<id>.o).
const CORES = fs.readdirSync(path.join(buildDir, "cores"))
  .filter((file) => file.endsWith(".o") && !file.endsWith("-merged.o"))
  .map((file) => path.basename(file, ".o"));

function run(exe, args, options = {}) {
  const result = spawnSync(exe, args, { encoding: "utf8", maxBuffer: 64 * 1024 * 1024, ...options });
  if (result.status !== 0 && !options.allowFailure) {
    process.stderr.write(result.stdout || "");
    process.stderr.write(result.stderr || "");
    throw new Error(`${path.basename(exe)} failed`);
  }
  return result;
}

// The exact compile command of a kernel source, from build.mjs' stamp files,
// without the dependency file options (those would overwrite the kernel's).
function kernelCommand(sourceName) {
  const objDir = path.join(buildDir, "obj");
  for (const stamp of fs.readdirSync(objDir).filter((file) => file.endsWith(".cmd"))) {
    const [exe, args] = JSON.parse(fs.readFileSync(path.join(objDir, stamp), "utf8"));
    if (args[args.length - 1].endsWith(sourceName)) {
      return [exe, args.filter((arg, i) => arg !== "-MMD" && arg !== "-MF" && args[i - 1] !== "-MF")];
    }
  }
  throw new Error(`No kernel build of ${sourceName}; run the kernel build for ${boardArg || "--board=gpi"} first`);
}

function findQemu() {
  const names = aarch64 ? ["qemu-aarch64", "qemu-aarch64-static"] : ["qemu-arm", "qemu-arm-static"];
  for (const name of names) {
    if (run("sh", ["-c", `command -v ${name}`], { allowFailure: true }).status === 0) return name;
  }
  throw new Error(`${names[0]} not found`);
}

const args = process.argv.slice(2);
const useFatfs = args.includes("--fatfs");
const optionsArg = args.find((arg) => arg.startsWith("--options="));
const optionsFile = optionsArg ? path.resolve(optionsArg.slice("--options=".length)) : null;
const useSequence = args.includes("--sequence");
const useSaves = args.includes("--saves");
const useInput = args.includes("--input");
const romArgs = args.filter((arg) => !["--fatfs", "--sequence", "--saves", "--input"].includes(arg)
  && arg !== optionsArg && arg !== boardArg);
// MAME finds the driver by the name of the zip file, so its test ROM set is
// invaders.zip, not test.zip.
const ROM_EXTENSIONS = [".nes", ".gb", ".gbc", ".gba", ".sfc", ".md", ".sms", ".gg",
  ".a26", ".lnx", ".pce", ".ws", ".wsc", ".sna", ".tap", ".z80", ".p", ".dsk", ".d64", ".p8", ".zip"];
const SAVE_EXTENSIONS = [".srm", ".rtc"];
const NOT_LISTED = ["cover.png", "test.gb.png", "other.srm", "minerva.log"];
if (optionsFile && !useFatfs) throw new Error("--options needs --fatfs");

fs.mkdirSync(outDir, { recursive: true });
const [cxx, runnerArgs] = kernelCommand(path.join("libretro", "libretro_runner.cpp"));
const gcc = cxx.replace(/g\+\+(\.exe)?$/, "gcc$1");
const cpuFlags = runnerArgs.filter((arg) => /^-m(cpu|fpu|float-abi|arm)/.test(arg) || arg === "-marm");

function compileLike(args, source, object, extra = []) {
  const out = args.slice(0, -4).filter((arg) => arg !== "-ffreestanding");
  run(args === runnerArgs ? cxx : gcc, [...out, ...extra, "-c", "-o", object, source]);
}

const objects = [];
const circleAddon = path.resolve(projectRoot, "..", "circle", "addon");
compileLike(runnerArgs, path.join(projectRoot, "libretro", "libretro_runner.cpp"), path.join(outDir, "runner.o"));
objects.push(path.join(outDir, "runner.o"));
compileLike(runnerArgs, path.join(testsDir, "smoke_main.cpp"), path.join(outDir, "smoke_main.o"),
  ["-DSMOKE_RUN_CONSTRUCTORS", ...(useFatfs ? ["-DSMOKE_FATFS"] : [])]);
objects.push(path.join(outDir, "smoke_main.o"));

if (useFatfs) {
  // The kernel's own newlib_glue.cpp and FatFs, plus a test environment for
  // what Circle provides (heap, disk access to the image file).
  const [, glueArgs] = kernelCommand(path.join("libc", "newlib_glue.cpp"));
  run(cxx, [...glueArgs.slice(0, -4), "-c", "-o", path.join(outDir, "glue.o"), path.join(projectRoot, "libc", "newlib_glue.cpp")]);
  objects.push(path.join(outDir, "glue.o"));
  for (const file of ["ff.c", "ffunicode.c"]) {
    const [ffCc, ffArgs] = kernelCommand(path.join("fatfs", file));
    run(ffCc, [...ffArgs.slice(0, -4), "-c", "-o", path.join(outDir, `${file}.o`), path.join(circleAddon, "fatfs", file)]);
    objects.push(path.join(outDir, `${file}.o`));
  }
  run(gcc, [...cpuFlags, "-O2", "-I", circleAddon, "-c", "-o", path.join(outDir, "fatfs_env.o"), path.join(testsDir, "arm_fatfs_env.c")]);
  objects.push(path.join(outDir, "fatfs_env.o"));
  // The kernel's file layer (saves: written as .tmp, then renamed).
  const [, fsArgs] = kernelCommand(path.join("platform", "circle", "circle_fs.cpp"));
  run(cxx, [...fsArgs.slice(0, -4).filter((arg) => arg !== "-ffreestanding"), "-c", "-o",
    path.join(outDir, "circle_fs.o"), path.join(projectRoot, "platform", "circle", "circle_fs.cpp")]);
  objects.push(path.join(outDir, "circle_fs.o"));
} else {
  run(gcc, [...cpuFlags, "-O2", "-I", path.join(projectRoot, "libc", "include"), "-c", "-o",
    path.join(outDir, "shim.o"), path.join(testsDir, "arm_linux_shim.c")]);
  objects.push(path.join(outDir, "shim.o"));
}

// AArch64: program start and newlib system calls of our own (no linux.specs
// for aarch64-none-elf); with --fatfs the glue's come first and win.
// crtbegin.o/crtend.o register .eh_frame for C++ exceptions, as in the kernel.
const crtFile = (name) => run(gcc, [...cpuFlags, `-print-file-name=${name}`]).stdout.trim();
if (aarch64) {
  run(gcc, [...cpuFlags, "-O2", "-c", "-o", path.join(outDir, "crt.o"), path.join(testsDir, "aarch64_linux_crt.c")]);
  objects.unshift(crtFile("crtbegin.o"));
  objects.push(path.join(outDir, "crt.o"));
}

const binary = path.join(outDir, useFatfs ? "smoke-fatfs" : "smoke");
// --gc-sections like the kernel link: it drops code the cores never call.
// AArch64: the libraries explicitly, so that crtend.o (the end marker of
// .eh_frame) comes after all of them.
run(cxx, [...cpuFlags, ...(aarch64 ? ["-nostdlib", "-Wl,--gc-sections"] : ["-specs=linux.specs"]), "-static", "-o", binary, ...objects,
  ...CORES.map((core) => path.join(buildDir, "cores", `${core}.o`)), "-lm",
  ...(aarch64 ? ["-Wl,--start-group", "-lstdc++", "-lm", "-lc", "-lgcc", "-Wl,--end-group", crtFile("crtend.o")] : []),
  // Like the kernel: the glue's syscalls/free and the test heap win over
  // libgloss-linux and newlib.
  ...(useFatfs ? ["-Wl,--allow-multiple-definition"] : [])]);

let image = null;
const imageDir = path.join(outDir, "image");
if (useFatfs) {
  image = path.join(outDir, "roms.img");
  fs.rmSync(imageDir, { recursive: true, force: true });
  fs.cpSync(romDir, imageDir, { recursive: true, filter: (file) => !SAVE_EXTENSIONS.includes(path.extname(file)) });
  if (optionsFile) fs.copyFileSync(optionsFile, path.join(imageDir, "minerva.cfg"));
  // Pictures, saves and logs next to the games: the ROM browser lists none
  // of them (they used to take the places of games).
  for (const file of NOT_LISTED) fs.writeFileSync(path.join(imageDir, file), "x");
  buildImage(image, imageDir);
}

const qemu = findQemu();
// With --fatfs the ROMs are paths inside the image.
// Without ROM arguments: the test.* ROMs, the save-* ROMs with --saves, the
// input-* ROMs with --input.
const romPrefix = useSaves ? "save-" : useInput ? "input-" : "test.";
const roms = romArgs.length
  ? romArgs.map((rom) => (useFatfs ? path.basename(rom) : path.resolve(rom)))
  : fs.readdirSync(romDir).sort()
    .filter((rom) => ROM_EXTENSIONS.includes(path.extname(rom))
      // MAME finds its zip through a directory listing: FatFs mode only.
      && (rom.startsWith(romPrefix) || (romPrefix === "test." && useFatfs && rom === "invaders.zip")))
    .map((rom) => (useFatfs ? rom : path.join(romDir, rom)));
const frames = process.env.SMOKE_FRAMES || "300";
const env = { ...process.env, ...(image ? { SMOKE_IMAGE: image } : {}) };
// Result lines of smoke_main start with the ROM's file name; with --fatfs
// stdout goes through the libc layer's log, which adds "[libc]".
const clean = (line) => line.replace(/^\s*\[libc\]\s*/, "").trim();
const resultLine = (line, name) => clean(line).startsWith(name);

let failed = 0;
const single = new Map();

// The save file of a ROM: next to it, or inside the image with --fatfs.
function saveFile(rom, extension) {
  const name = `${path.basename(rom, path.extname(rom))}${extension}`;
  if (!useFatfs) {
    const file = path.join(path.dirname(rom), name);
    return fs.existsSync(file) ? fs.readFileSync(file) : null;
  }
  const result = spawnSync("mcopy", ["-i", `${image}@@${2048 * 512}`, `::/${name}`, "-"], { maxBuffer: 4 * 1024 * 1024 });
  return result.status === 0 ? result.stdout : null;
}

if (useSaves) {
  if (!useFatfs) {
    for (const rom of roms) {
      for (const extension of SAVE_EXTENSIONS) {
        fs.rmSync(path.join(path.dirname(rom), `${path.basename(rom, path.extname(rom))}${extension}`), { force: true });
      }
    }
  }
  for (const rom of roms) {
    const name = path.basename(rom);
    const saves = [];
    for (let pass = 0; pass < 2; pass++) {
      const result = run(qemu, ["-cpu", qemuCpu, binary, frames, rom], { allowFailure: true, timeout: 600000, env });
      if (result.status !== 0) {
        console.log(`${name}: run ${pass + 1} failed (exit ${result.status})\n${result.stdout}${result.stderr}`);
      }
      saves.push(saveFile(rom, ".srm"));
    }
    const [first, second] = saves;
    let verdict = "FAIL";
    let detail;
    if (!first || !second) {
      detail = `no save file after run ${first ? 2 : 1}`;
    } else if (first.length !== second.length) {
      detail = `save size changed from ${first.length} to ${second.length} bytes`;
    } else {
      const changed = [...first.keys()].filter((i) => first[i] !== second[i]);
      if (changed.length === 1 && second[changed[0]] === ((first[changed[0]] + 1) & 0xff)) {
        verdict = "ok";
        detail = `${first.length} bytes, byte ${changed[0]}: ${first[changed[0]]} -> ${second[changed[0]]}`;
      } else {
        detail = `${changed.length} bytes differ between the runs`;
      }
    }
    const rtc = saveFile(rom, ".rtc");
    if (rtc) detail += `, clock ${rtc.length} bytes`;
    console.log(`${verdict.padEnd(5)} ${name.padEnd(14)} ${detail}`);
    if (verdict !== "ok") failed++;
  }
  process.exit(failed ? 1 : 0);
}

if (useInput) {
  // RetroPad masks: B 0x1, Y 0x2, Select 0x4, Start 0x8, A 0x100, X 0x200.
  // Expected: the colour word the ROM writes, i.e. the console's buttons.
  // SNES: JOY1L | JOY1H << 8 (A 0x80, X 0x40, L 0x20, R 0x10 | B, Y, Select
  // 0x2000, Start 0x1000). GBA: KEYINPUT (A 0x1, Select 0x4, R 0x100, L 0x200).
  const cases = [
    ["input-snes.sfc", "nothing", "0", 30, 0x0000],
    ["input-snes.sfc", "X is X", "0x200", 30, 0x0040],
    ["input-snes.sfc", "Y is Y", "0x2", 30, 0x4000],
    ["input-snes.sfc", "Select+X is R, no Select", "0x204", 30, 0x0010],
    ["input-snes.sfc", "Select+Y is L, no Select", "0x6", 30, 0x0020],
    ["input-snes.sfc", "Select held reaches the game", "0x4", 30, 0x2000],
    ["input-snes.sfc", "Select tapped reaches the game", "0x4@3,0", 7, 0x2000],
    ["input-snes.sfc", "Select+X tapped: no Select", "0x204@3,0", 7, 0x0000],
    ["input-snes.sfc", "Start+Select: Start", "0xc", 10, 0x1000],
    ["input-gba.gba", "Y is L", "0x2", 30, 0x200],
    ["input-gba.gba", "X is R", "0x200", 30, 0x100],
    ["input-gba.gba", "A is A", "0x100", 30, 0x001],
    ["input-gba.gba", "Select at once", "0x4", 5, 0x004],
  ];
  for (const [name, what, buttons, caseFrames, expected] of cases) {
    const rom = roms.find((candidate) => path.basename(candidate) === name);
    if (!rom) throw new Error(`${name} missing, run make-test-roms.mjs`);
    const result = run(qemu, ["-cpu", qemuCpu, binary, String(caseFrames), rom],
      { allowFailure: true, timeout: 600000, env: { ...env, SMOKE_BUTTONS: buttons } });
    const line = `${result.stdout || ""}${result.stderr || ""}`.split("\n").map(clean).find((candidate) => candidate.startsWith(name)) || "";
    const match = line.match(/center=#([0-9a-f]{6})/);
    // Back from the RGB565 picture to the 15-bit colour word (5 bits each).
    const rgb = match ? Number.parseInt(match[1], 16) : -1;
    const word = rgb < 0 ? -1 : ((rgb >> 19) & 31) | (((rgb >> 11) & 31) << 5) | (((rgb >> 3) & 31) << 10);
    const ok = word === expected;
    console.log(`${ok ? "ok   " : "FAIL "} ${name.padEnd(15)} ${what.padEnd(32)} buttons ${buttons.padEnd(10)} console 0x${word.toString(16).padStart(4, "0")}${ok ? "" : ` (expected 0x${expected.toString(16).padStart(4, "0")})`}`);
    if (!ok) failed++;
  }
  process.exit(failed ? 1 : 0);
}

for (const rom of roms) {
  const result = run(qemu, ["-cpu", qemuCpu, binary, frames, rom], { allowFailure: true, timeout: 600000, env });
  const output = `${result.stdout || ""}${result.stderr || ""}`.trim();
  console.log(output.split("\n").filter((line) => !/polyphase|\] listed |^listed /.test(line)).join("\n") || `${path.basename(rom)}: no output (exit ${result.status}, signal ${result.signal})`);
  const name = path.basename(rom);
  const line = output.split("\n").find((candidate) => resultLine(candidate, name));
  single.set(name, line && clean(line));
  if (result.status !== 0) failed++;
  if (useFatfs && rom === roms[0]) {
    const listed = output.split("\n").map(clean).filter((candidate) => candidate.startsWith("listed "))
      .map((candidate) => candidate.slice("listed ".length));
    const games = fs.readdirSync(imageDir).filter((file) => !NOT_LISTED.includes(file) && file !== "minerva.cfg");
    const ok = games.every((file) => listed.includes(file)) && !listed.some((file) => NOT_LISTED.includes(file));
    console.log(`${ok ? "ok  " : "FAIL"}  browser listing: ${listed.length} entries${ok ? "" : ` (${listed.join(" ")})`}`);
    if (!ok) failed++;
  }
}

if (useSequence) {
  const sequence = [...roms, ...roms];
  console.log(`\nSequence in one process: ${sequence.map((rom) => path.basename(rom)).join(", ")}`);
  const result = run(qemu, ["-cpu", qemuCpu, binary, frames, ...sequence], { allowFailure: true, timeout: 1200000, env });
  const lines = `${result.stdout || ""}${result.stderr || ""}`.split("\n");
  let position = 0;
  for (const rom of sequence) {
    const name = path.basename(rom);
    const index = lines.findIndex((line, i) => i >= position && resultLine(line, name));
    const line = index >= 0 ? clean(lines[index]) : null;
    if (index >= 0) position = index + 1;
    const ok = line && line === single.get(name);
    console.log(`${ok ? "same" : "DIFF"}  ${line || `${name}: no result (exit ${result.status}, signal ${result.signal})`}`);
    if (!ok) {
      console.log(`      alone: ${single.get(name)}`);
      failed++;
    }
  }
}
process.exit(failed ? 1 : 0);
