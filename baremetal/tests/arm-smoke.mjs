#!/usr/bin/env node
// Runs the bundled cores as built for the kernel (baremetal/build-node/gpi-all/
// cores/*.o, including gpSP's ARM dynarec) on an emulated ARM1176 under
// qemu-arm, as an ARM Linux program driven by smoke_main.cpp.
//
// Needs: a finished "npm run build:gpi", qemu-arm (or qemu-arm-static) and the
// test ROMs from make-test-roms.mjs.
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
// usage: node baremetal/tests/arm-smoke.mjs [--fatfs [--options=<file>]] [--sequence | --saves | --input] [rom ...]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { buildImage } from "../sdcard.mjs";

const testsDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(testsDir, "..");
const buildDir = path.join(projectRoot, "build-node", "gpi-all");
const outDir = path.join(projectRoot, "build-node", "arm-smoke");
const romDir = path.join(projectRoot, "build-node", "test-roms");
const CORES = ["fceumm", "gambatte", "snes9x2002", "picodrive", "gpsp"];

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
  throw new Error(`No kernel build of ${sourceName}; run "npm run build:gpi" first`);
}

function findQemu() {
  for (const name of ["qemu-arm", "qemu-arm-static"]) {
    if (run("sh", ["-c", `command -v ${name}`], { allowFailure: true }).status === 0) return name;
  }
  throw new Error("qemu-arm not found");
}

const args = process.argv.slice(2);
const useFatfs = args.includes("--fatfs");
const optionsArg = args.find((arg) => arg.startsWith("--options="));
const optionsFile = optionsArg ? path.resolve(optionsArg.slice("--options=".length)) : null;
const useSequence = args.includes("--sequence");
const useSaves = args.includes("--saves");
const useInput = args.includes("--input");
const romArgs = args.filter((arg) => !["--fatfs", "--sequence", "--saves", "--input"].includes(arg) && arg !== optionsArg);
const ROM_EXTENSIONS = [".nes", ".gb", ".gbc", ".gba", ".sfc", ".md", ".sms", ".gg"];
const SAVE_EXTENSIONS = [".srm", ".rtc"];
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

const binary = path.join(outDir, useFatfs ? "smoke-fatfs" : "smoke");
run(cxx, [...cpuFlags, "-specs=linux.specs", "-static", "-o", binary, ...objects,
  ...CORES.map((core) => path.join(buildDir, "cores", `${core}.o`)), "-lm",
  // Like the kernel: the glue's syscalls/free and the test heap win over
  // libgloss-linux and newlib.
  ...(useFatfs ? ["-Wl,--allow-multiple-definition"] : [])]);

let image = null;
if (useFatfs) {
  image = path.join(outDir, "roms.img");
  const imageDir = path.join(outDir, "image");
  fs.rmSync(imageDir, { recursive: true, force: true });
  fs.cpSync(romDir, imageDir, { recursive: true, filter: (file) => !SAVE_EXTENSIONS.includes(path.extname(file)) });
  if (optionsFile) fs.copyFileSync(optionsFile, path.join(imageDir, "minerva.cfg"));
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
    .filter((rom) => ROM_EXTENSIONS.includes(path.extname(rom)) && rom.startsWith(romPrefix))
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
      const result = run(qemu, ["-cpu", "arm1176", binary, frames, rom], { allowFailure: true, timeout: 600000, env });
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
    const result = run(qemu, ["-cpu", "arm1176", binary, String(caseFrames), rom],
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
  const result = run(qemu, ["-cpu", "arm1176", binary, frames, rom], { allowFailure: true, timeout: 600000, env });
  const output = `${result.stdout || ""}${result.stderr || ""}`.trim();
  console.log(output.split("\n").filter((line) => !/polyphase/.test(line)).join("\n") || `${path.basename(rom)}: no output (exit ${result.status}, signal ${result.signal})`);
  const name = path.basename(rom);
  const line = output.split("\n").find((candidate) => resultLine(candidate, name));
  single.set(name, line && clean(line));
  if (result.status !== 0) failed++;
}

if (useSequence) {
  const sequence = [...roms, ...roms];
  console.log(`\nSequence in one process: ${sequence.map((rom) => path.basename(rom)).join(", ")}`);
  const result = run(qemu, ["-cpu", "arm1176", binary, frames, ...sequence], { allowFailure: true, timeout: 1200000, env });
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
