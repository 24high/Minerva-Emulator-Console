#!/usr/bin/env node
import { spawnSync } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

import { assembleSdCard } from "./sdcard.mjs";
import { writeSplashSource } from "./splash.mjs";

const repoRoot = path.resolve(path.join(path.dirname(fileURLToPath(import.meta.url)), ".."));
const projectRoot = path.join(repoRoot, "baremetal");

const cli = new Map();
for (const arg of process.argv.slice(2)) {
  const match = arg.match(/^--([^=]+)=(.*)$/);
  if (match) cli.set(match[1], match[2]);
}

// Each board selects the Circle configuration (RASPPI/AARCH), the toolchain
// and the hardware glue that is compiled into the kernel.
const boards = {
  pi5: {
    label: "Raspberry Pi 5",
    aarch: 64,
    rasppi: 5,
    triple: "aarch64-none-elf",
    defaultToolchain: process.platform === "win32"
      ? "C:/Users/dennis/Documents/GitHub/MINERVA_closed/fantasy-pi/toolchain/aarch64-none-elf"
      : null,
    cpuFlags: ["-mcpu=cortex-a76", "-mlittle-endian"],
    alignFlags: ["-mstrict-align"],
    target: "kernel_2712",
    loadAddress: "0x80000",
    screen: { width: 1920, height: 1080 },
    buildDirPrefix: "",
    defaultCore: "pattern",
    cores: ["pattern", "boottest", "boottest-fceumm", "fceumm", "n64", "multi"],
    defines: [],
  },
  gpi: {
    label: "Raspberry Pi Zero / Zero W im Retroflag GPi Case",
    aarch: 32,
    rasppi: 1,
    triple: "arm-none-eabi",
    defaultToolchain: null,
    cpuFlags: ["-mcpu=arm1176jzf-s", "-marm", "-mfpu=vfp", "-mfloat-abi=hard"],
    alignFlags: ["-mno-unaligned-access"],
    target: "kernel",
    loadAddress: "0x8000",
    screen: { width: 320, height: 240 },
    buildDirPrefix: "gpi-",
    defaultCore: "all",
    // "all" bundles NES, Game Boy (Advance), SNES and Mega Drive/Master System. The
    // boottest kernels drive the UART on GPIO14/15, which are DPI data lines
    // in the GPi Case. N64 needs an AArch64 dynarec and a multi-core CPU.
    cores: ["all", "fceumm"],
    // Circle's FatFs addon instead of its root-directory-only FAT driver.
    fatfs: true,
    // Project copies that replace Circle library sources (path relative to circle/lib).
    circleOverrides: {
      // The GPi controller poses as an Xbox 360 pad but never acknowledges the
      // LED command, which Circle sends without timeout during USB init.
      "usb/usbgamepadxbox360.cpp": path.join(projectRoot, "platform", "circle", "overrides", "usbgamepadxbox360.cpp"),
    },
    defines: [
      "-DRA_BAREMETAL_GPI_CASE=1",
      "-DRA_BAREMETAL_FATFS=1",
      // PWM audio on GPIO18 (left) and GPIO19 (right), as wired in the GPi Case.
      "-DUSE_PWM_AUDIO_ON_ZERO",
      "-DUSE_GPIO18_FOR_LEFT_PWM_ON_ZERO",
      "-DUSE_GPIO19_FOR_RIGHT_PWM_ON_ZERO",
      // Circle saves the VFP registers on interrupts only for RASPPI >= 2, but
      // GCC may use them in any function, including the IRQ/FIQ handlers.
      "-DSAVE_VFP_REGS_ON_IRQ",
      "-DSAVE_VFP_REGS_ON_FIQ",
      // Circle's heap never reuses freed blocks above its largest bucket
      // (512 KB by default). Games can be ended and others started (Start+
      // Select), and the cores allocate up to 32 MB per block (gpSP: ROM in
      // 1 MB blocks, 10.5 MB translation cache), so add buckets up to 32 MB.
      "-DHEAP_BLOCK_BUCKET_SIZES=0x40,0x400,0x1000,0x4000,0x10000,0x40000,0x80000,"
        + "0x100000,0x200000,0x400000,0x800000,0x1000000,0x2000000",
    ],
    sdCard: {
      configDir: path.join(projectRoot, "gpi"),
      outputDir: path.join(repoRoot, "output-gpi"),
      image: path.join(repoRoot, "dist", "minerva-gpi-zero.img"),
      firmware: ["bootcode.bin", "start.elf", "fixup.dat", "LICENCE.broadcom"],
    },
  },
};

const selectedBoardName = (cli.get("board") || process.env.BOARD || "pi5").toLowerCase();
const board = boards[selectedBoardName];
if (!board) {
  console.error(`\nERROR: Unknown board '${selectedBoardName}'. Use ${Object.keys(boards).map((name) => `'${name}'`).join(" or ")}.`);
  process.exit(1);
}

const exeSuffix = process.platform === "win32" ? ".exe" : "";
const selectedCore = (cli.get("core") || process.env.CORE || board.defaultCore).toLowerCase();
const selectedN64FrameSkip = cli.get("n64-frameskip") || process.env.N64_FRAMESKIP || "3";
const selectedN64PresentDivisor = cli.get("n64-vi-divisor")
  || process.env.N64_VI_DIVISOR
  || String(Math.max(1, Number.parseInt(selectedN64FrameSkip, 10) + 1 || 1));

function findToolchain() {
  if (process.env.TOOLCHAIN) return path.resolve(process.env.TOOLCHAIN);
  if (board.defaultToolchain) return path.resolve(board.defaultToolchain);

  // Prefer an unpacked Arm GNU toolchain in <repo>/toolchain, then the PATH.
  const local = path.join(repoRoot, "toolchain");
  if (fs.existsSync(local)) {
    const match = fs.readdirSync(local).sort().reverse().find((dir) => dir.endsWith(board.triple));
    if (match) return path.join(local, match);
  }
  for (const dir of (process.env.PATH || "").split(path.delimiter)) {
    if (dir && fs.existsSync(path.join(dir, `${board.triple}-gcc${exeSuffix}`))) {
      return path.dirname(dir);
    }
  }
  return path.join(local, `arm-gnu-toolchain-${board.triple}`);
}

// Boot image: --splash=<png> or SPLASH=<png>, default baremetal/assets/splash.png;
// --splash=0 brings back the text boot log.
function splashImage() {
  const option = cli.get("splash") ?? process.env.SPLASH;
  if (option === "0") return null;
  const file = option ? path.resolve(option) : path.join(projectRoot, "assets", "splash.png");
  if (option && !fs.existsSync(file)) {
    console.error(`\nERROR: Missing splash image: ${file}`);
    process.exit(1);
  }
  return fs.existsSync(file) ? file : null;
}

function circleVersion(circleHome) {
  const rules = path.join(circleHome, "Rules.mk");
  const match = fs.existsSync(rules) ? fs.readFileSync(rules, "utf8").match(/^CIRCLEVER\s*=\s*(\d+)/m) : null;
  return match ? match[1] : "510000";
}

const circleHome = path.resolve(process.env.CIRCLEHOME || path.join(repoRoot, "circle"));

const config = {
  boardName: selectedBoardName,
  circleHome,
  circleVersion: circleVersion(circleHome),
  toolchain: findToolchain(),
  core: selectedCore,
  romPath: cli.get("rom") || process.env.ROM_PATH || (selectedCore === "fceumm" || selectedCore === "multi" ? "GAME.NES" : selectedCore === "n64" ? "GAME.Z64" : "GAME.GB"),
  buildDir: path.join(projectRoot, "build-node", `${board.buildDirPrefix}${selectedCore}`),
  target: board.target,
  optimize: process.env.OPTIMIZE || "-O3",
  noUsb: selectedCore === "fceumm" && (cli.get("usb") === "0" || process.env.USB === "0"),
  kernelMaxSize: cli.get("kernel-max-size") || process.env.KERNEL_MAX_SIZE || (selectedCore === "n64" || selectedCore === "multi" ? "0x08000000" : selectedCore === "all" ? "0x1000000" : selectedCore === "fceumm" ? "0x800000" : "0x200000"),
  hdmiPhysicalWidth: cli.get("hdmi-physical-width") || process.env.HDMI_PHYSICAL_WIDTH || "1920",
  hdmiPhysicalHeight: cli.get("hdmi-physical-height") || process.env.HDMI_PHYSICAL_HEIGHT || "1080",
  n64FrameSkip: selectedN64FrameSkip,
  n64ViDivisor: selectedN64PresentDivisor,
  sdCard: board.sdCard && cli.get("sd") !== "0" && process.env.SD !== "0",
  splash: splashImage(),
};

if (!board.cores.includes(config.core)) {
  console.error(`\nERROR: Core '${config.core}' is not available for board '${config.boardName}'. Use ${board.cores.map((name) => `'${name}'`).join(", ")}.`);
  process.exit(1);
}

const bin = path.join(config.toolchain, "bin");
const toolPrefix = path.join(bin, `${board.triple}-`);
const tools = {
  cc: `${toolPrefix}gcc${exeSuffix}`,
  cxx: `${toolPrefix}g++${exeSuffix}`,
  ld: `${toolPrefix}ld${exeSuffix}`,
  ar: `${toolPrefix}ar${exeSuffix}`,
  objcopy: `${toolPrefix}objcopy${exeSuffix}`,
  objdump: `${toolPrefix}objdump${exeSuffix}`,
  cxxfilt: `${toolPrefix}c++filt${exeSuffix}`,
};

function fail(message) {
  console.error(`\nERROR: ${message}`);
  process.exit(1);
}

function ensureFile(file) {
  if (!fs.existsSync(file)) {
    fail(`Missing file: ${file}`);
  }
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function run(label, exe, args, options = {}) {
  ensureFile(exe);
  console.log(`${label.padEnd(7)} ${options.display || ""}`);
  const result = spawnSync(exe, args, {
    cwd: options.cwd || repoRoot,
    encoding: "utf8",
    stdio: options.capture ? "pipe" : "inherit",
    maxBuffer: 64 * 1024 * 1024,
  });

  if (result.status !== 0) {
    if (options.capture) {
      if (result.stdout) process.stdout.write(result.stdout);
      if (result.stderr) process.stderr.write(result.stderr);
    }
    fail(`${label.trim()} failed: ${exe} ${args.join(" ")}`);
  }

  return result.stdout || "";
}

function outputOf(exe, args) {
  return run("QUERY", exe, args, { capture: true }).trim().replace(/^"|"$/g, "");
}

function newer(target, inputs) {
  if (!fs.existsSync(target)) return true;
  const targetTime = fs.statSync(target).mtimeMs;
  // A missing input (e.g. a header that was removed) also means rebuild.
  return inputs.some((input) => !fs.existsSync(input) || fs.statSync(input).mtimeMs > targetTime);
}

// Source and headers of an object, from the dependency file the compiler
// writes with -MMD; null if there is none yet.
function dependenciesOf(depFile) {
  if (!fs.existsSync(depFile)) return null;
  const text = fs.readFileSync(depFile, "utf8").replace(/\\\r?\n/g, " ");
  const colon = text.indexOf(": ");
  if (colon < 0) return null;
  return (text.slice(colon + 2).match(/(?:\\ |\S)+/g) || []).map((dep) => dep.replace(/\\ /g, " "));
}

function relFromRoot(file) {
  return path.relative(repoRoot, file).replace(/\\/g, "/");
}

function objPathFor(source) {
  const rel = relFromRoot(source).replace(/[:]/g, "");
  const hash = crypto.createHash("sha1").update(rel).digest("hex").slice(0, 12);
  const base = path.basename(source).replace(/[^A-Za-z0-9_.-]/g, "_");
  return path.join(config.buildDir, "obj", `${hash}_${base}.o`);
}

function archivePath(name) {
  return path.join(config.buildDir, "lib", name);
}

function prefixed(base, items, ext = ".cpp") {
  return items.map((item) => path.join(base, item.endsWith(".S") || item.endsWith(".c") || item.endsWith(".cpp") ? item : `${item}${ext}`));
}

function existingSources(base, names) {
  return names.map((name) => {
    const source = path.join(base, name);
    ensureFile(source);
    return source;
  });
}

function compile(source, extraFlags = [], options = {}) {
  const object = objPathFor(source);
  ensureDir(path.dirname(object));

  const ext = path.extname(source).toLowerCase();
  const isCxx = ext === ".cpp" || ext === ".cc";
  const isAsm = ext === ".s";
  // .S goes through the preprocessor and can include headers, .s does not.
  const preprocessed = !isAsm || path.extname(source) === ".S";
  const exe = isCxx ? tools.cxx : tools.cc;
  const std = isCxx
    ? options.cxxStd ?? ["-std=c++17", "-fno-exceptions", "-fno-rtti", "-nostdinc++"]
    : ext === ".c" ? options.cStd ?? ["-std=gnu99"] : [];
  const args = [
    ...(options.prependFlags || []),
    ...(options.baseFlags || commonCompileFlags),
    ...std,
    ...extraFlags,
    ...(options.fileFlags ? options.fileFlags(source) : []),
    // Records the headers; a changed header must rebuild every object that
    // includes it (a class layout differing between objects corrupts memory).
    ...(preprocessed ? ["-MMD", "-MF", `${object}.d`] : []),
    "-c",
    "-o",
    object,
    source,
  ];

  const stamp = `${object}.cmd`;
  const commandKey = JSON.stringify([exe, args]);
  const dependencies = preprocessed ? dependenciesOf(`${object}.d`) : [source];
  if (dependencies && !newer(object, dependencies)
      && fs.existsSync(stamp) && fs.readFileSync(stamp, "utf8") === commandKey) {
    return object;
  }

  run(isAsm ? "AS" : isCxx ? "CXX" : "CC", exe, args, { display: relFromRoot(source) });
  fs.writeFileSync(stamp, commandKey);
  return object;
}

function archive(name, sources, extraFlags = [], options = {}) {
  const output = archivePath(name);
  ensureDir(path.dirname(output));
  const objects = sources.map((source) => compile(source, extraFlags, options));

  if (!newer(output, objects)) {
    return output;
  }

  if (fs.existsSync(output)) fs.unlinkSync(output);
  const chunkSize = 80;
  for (let i = 0; i < objects.length; i += chunkSize) {
    const chunk = objects.slice(i, i + chunkSize);
    run("AR", tools.ar, [i === 0 ? "cr" : "r", output, ...chunk], {
      display: i === 0 ? relFromRoot(output) : `${relFromRoot(output)} +${i}`,
    });
  }
  return output;
}

function parseLibs() {
  // Ask the compiler so AArch32 builds get the matching multilib (e.g. arm/v5te/hard).
  const libgcc = outputOf(tools.cc, [...board.cpuFlags, "-print-file-name=libgcc.a"]);
  const libm = outputOf(tools.cc, [...board.cpuFlags, "-print-file-name=libm.a"]);
  const libc = path.resolve(outputOf(tools.cc, [...board.cpuFlags, "-print-file-name=libc.a"]));
  const libnosys = path.resolve(outputOf(tools.cc, [...board.cpuFlags, "-print-file-name=libnosys.a"]));
  const libstdcxx = path.resolve(outputOf(tools.cxx, [...board.cpuFlags, "-print-file-name=libstdc++.a"]));
  ensureFile(libgcc);
  ensureFile(libm);
  ensureFile(libc);
  ensureFile(libnosys);
  return { libgcc, libm, libc, libnosys, libstdcxx };
}

function listSources(dir, extension = ".c") {
  return fs.readdirSync(dir)
    .filter((file) => file.endsWith(extension))
    .sort()
    .map((file) => path.join(dir, file));
}

const libretroApiSymbols = [
  "retro_init",
  "retro_deinit",
  "retro_api_version",
  "retro_get_system_info",
  "retro_get_system_av_info",
  "retro_set_environment",
  "retro_set_video_refresh",
  "retro_set_audio_sample",
  "retro_set_audio_sample_batch",
  "retro_set_input_poll",
  "retro_set_input_state",
  "retro_set_controller_port_device",
  "retro_reset",
  "retro_run",
  "retro_serialize_size",
  "retro_serialize",
  "retro_unserialize",
  "retro_cheat_reset",
  "retro_cheat_set",
  "retro_load_game",
  "retro_load_game_special",
  "retro_unload_game",
  "retro_get_region",
  "retro_get_memory_data",
  "retro_get_memory_size",
];

function libretroSymbolDefines(prefix) {
  return libretroApiSymbols.map((symbol) => `-D${symbol}=${prefix}_${symbol}`);
}

function prefixedGlobalDefines(prefix, symbols) {
  return symbols.map((symbol) => `-D${symbol}=${prefix}_${symbol}`);
}

const multiCoreSharedSymbolDefines = [
  "md5_finish",
  "option_cats_us",
  "option_defs_us",
  "options_us",
  "string_trim_whitespace",
  "string_trim_whitespace_left",
  "string_trim_whitespace_right",
  "strlcat_retro__",
  "strlcpy_retro__",
];

const includeFlags = [
  "-I", path.join(config.circleHome, "include"),
  "-I", path.join(config.circleHome, "addon"),
  "-I", path.join(config.circleHome, "app", "lib"),
  "-I", path.join(config.circleHome, "addon", "vc4"),
  "-I", path.join(config.circleHome, "addon", "vc4", "interface", "khronos", "include"),
  "-I", projectRoot,
  "-I", path.join(projectRoot, "kernel"),
  "-I", path.join(projectRoot, "libretro"),
  "-I", path.join(projectRoot, "platform", "circle"),
  "-I", path.join(repoRoot, "src", "libretro-common", "include"),
  ...(config.core === "all" ? ["-I", path.join(projectRoot, "libc", "include")] : []),
];

const defines = [
  `-DAARCH=${board.aarch}`,
  `-DRASPPI=${board.rasppi}`,
  `-D__circle__=${config.circleVersion}`,
  "-DSTDLIB_SUPPORT=1",
  "-D__VCCOREVER__=0x04000000",
  "-DDEPTH=16",
  `-DKERNEL_MAX_SIZE=${config.kernelMaxSize}`,
  ...board.defines,
  ...(config.splash ? ["-DRA_BAREMETAL_SPLASH=1"] : []),
  `-DRA_BAREMETAL_HDMI_PHYSICAL_WIDTH=${config.hdmiPhysicalWidth}`,
  `-DRA_BAREMETAL_HDMI_PHYSICAL_HEIGHT=${config.hdmiPhysicalHeight}`,
  `-DRA_BAREMETAL_ROM_PATH="${config.romPath}"`,
  `-DRA_BAREMETAL_N64_FRAMESKIP=${config.n64FrameSkip}`,
  `-DRA_BAREMETAL_N64_VI_PRESENT_DIVISOR=${config.n64ViDivisor}`,
  `-DRA_BAREMETAL_MAX_ROM_SIZE=${config.core === "n64" || config.core === "multi" ? "0x4000000" : "0x1000000"}`,
  ...(config.core === "all" ? ["-DRA_BAREMETAL_CORE_BUNDLE=1"] : []),
  ...(config.core === "fceumm" ? ["-DRA_BAREMETAL_FCEUMM=1"] : []),
  ...(config.core === "n64" ? ["-DRA_BAREMETAL_N64=1"] : []),
  ...(config.core === "multi" ? ["-DRA_BAREMETAL_MULTI=1"] : []),
  ...(config.core === "n64" || config.core === "multi" ? ["-DARM_ALLOW_MULTI_CORE=1"] : []),
  ...(config.core === "n64" || config.core === "multi" ? ["-DRA_BAREMETAL_ENABLE_JIT=1"] : []),
  ...(config.noUsb ? ["-DRA_BAREMETAL_NO_USB=1"] : []),
  "-U__unix__",
  "-U__linux__",
];

const commonCompileFlags = [
  ...board.cpuFlags,
  "-Wall",
  "-fsigned-char",
  "-g",
  config.optimize,
  "-ffreestanding",
  ...board.alignFlags,
  ...defines,
  ...includeFlags,
];

// Inttypes format macros passed to the cores; int64_t is "long" on AArch64
// but "long long" on arm-none-eabi.
const formatDefines = board.aarch === 64
  ? { PRIX64: "lX", PRIxPTR: "lx", PRIuPTR: "lu", PRIu64: "lu", PRId64: "ld" }
  : { PRIX64: "llX", PRIxPTR: "x", PRIuPTR: "u", PRIu64: "llu", PRId64: "lld" };

function formatDefine(name) {
  return `-D${name}="${formatDefines[name]}"`;
}

function withCircleOverrides(sources) {
  const overrides = board.circleOverrides || {};
  return sources.map((source) => {
    const override = overrides[path.relative(circleLibBase, source).replace(/\\/g, "/")];
    if (!override) return source;
    ensureFile(override);
    return override;
  });
}

// Files that only exist in newer Circle releases are added when present.
function optionalSources(base, names) {
  return names.map((name) => path.join(base, name)).filter((source) => fs.existsSync(source));
}

// Source lists follow the Circle Makefiles (lib/Makefile, lib/usb/Makefile, ...)
// for the selected RASPPI/AARCH combination.
const circleLibBase = path.join(config.circleHome, "lib");
const circleCoreSources = existingSources(circleLibBase, [
  "actled.cpp", "alloc.cpp", "assert.cpp", "display.cpp", "windowdisplay.cpp",
  "bcmframebuffer.cpp", "bcmmailbox.cpp", "bcmpropertytags.cpp", "bcmwatchdog.cpp",
  "chargenerator.cpp", "classallocator.cpp", "cputhrottle.cpp", "debug.cpp",
  "delayloop.S", "device.cpp", "devicenameservice.cpp", "dmachannel.cpp",
  "koptions.cpp", "logger.cpp", "machineinfo.cpp", "multicore.cpp",
  "nulldevice.cpp", "ptrarray.cpp", "ptrlist.cpp", "qemu.cpp", "terminal.cpp",
  "screen.cpp", "serial.cpp", "spinlock.cpp", "string.cpp", "sysinit.cpp",
  "time.cpp", "timer.cpp", "tracer.cpp", "util.cpp", "util_fast.S",
  "virtualgpiopin.cpp", "chainboot.cpp", "macaddress.cpp", "netdevice.cpp",
  "new.cpp", "heapallocator.cpp", "pageallocator.cpp", "setjmp.S",
  "numberpool.cpp", "writebuffer.cpp", "2dgraphics.cpp", "ptrlistfiq.cpp",
  "font6x7.cpp", "font8x8.cpp", "font8x10.cpp", "font8x12.cpp",
  "font8x14.cpp", "font8x16.cpp", "font12x22.cpp",
  ...(board.aarch === 64
    ? [
      "exceptionhandler64.cpp", "exceptionstub64.S", "memory64.cpp", "startup64.S",
      "synchronize64.cpp", "translationtable64.cpp",
    ]
    : [
      "cache-v7.S", "exceptionhandler.cpp", "exceptionstub.S", "memory.cpp",
      "pagetable.cpp", "startup.S", "synchronize.cpp",
    ]),
  ...(board.rasppi <= 3
    ? ["bcmrandom.cpp", "interrupt.cpp", "mphi.cpp"]
    : ["bcmpciehostbridge.cpp", "bcmrandom200.cpp", "interruptgic.cpp", "dma4channel.cpp", "devicetreeblob.cpp"]),
  ...(board.rasppi === 5
    ? [
      "southbridge.cpp", "dmachannel-rp1.cpp", "gpiomanager2712.cpp", "gpiopin2712.cpp",
      "gpioclock-rp1.cpp", "pwmoutput-rp1.cpp", "i2cmaster-rp1.cpp",
      "spimaster-rp1.cpp", "spimasterdma-rp1.cpp", "macb.cpp",
    ]
    : [
      "gpioclock.cpp", "gpiomanager.cpp", "gpiopin.cpp", "gpiopinfiq.cpp", "i2cmaster.cpp",
      "i2cmasterirq.cpp", "i2cslave.cpp", "pwmoutput.cpp", "smimaster.cpp", "spimaster.cpp",
      "spimasteraux.cpp", "spimasterdma.cpp", "usertimer.cpp", "latencytester.cpp",
    ]),
  "purecall.cpp", "cxa_guard.cpp",
]);

const usbSources = withCircleOverrides([
  ...existingSources(path.join(circleLibBase, "usb"), [
    "lan7800.cpp", "smsc951x.cpp", "usbbluetooth.cpp", "usbcdcethernet.cpp",
    "usbfloppydevice.cpp", "usbconfigparser.cpp", "usbdevice.cpp",
    "usbdevicefactory.cpp", "usbendpoint.cpp", "usbfunction.cpp",
    "usbgamepad.cpp", "usbgamepadps3.cpp", "usbgamepadps4.cpp",
    "usbgamepadstandard.cpp", "usbgamepadswitchpro.cpp", "usbgamepadxbox360.cpp",
    "usbgamepadxboxone.cpp", "usbhiddevice.cpp", "usbhostcontroller.cpp",
    "usbkeyboard.cpp", "usbmassdevice.cpp", "usbmidi.cpp", "usbmidihost.cpp",
    "usbmouse.cpp", "usbprinter.cpp", "usbrequest.cpp", "usbstandardhub.cpp",
    "usbstring.cpp", "usbserial.cpp", "usbserialhost.cpp", "usbserialch341.cpp",
    "usbserialcp210x.cpp", "usbserialpl2303.cpp", "usbserialft231x.cpp",
    "usbserialcdc.cpp", "usbtouchscreen.cpp", "dwhciregister.cpp",
    ...(board.rasppi <= 3
      ? [
        "dwhcidevice.cpp", "dwhciframeschednper.cpp", "dwhciframeschednsplit.cpp",
        "dwhciframeschedper.cpp", "dwhcirootport.cpp", "dwhcixactqueue.cpp",
        "dwhcicompletionqueue.cpp", "dwhcixferstagedata.cpp", "dwhciframeschediso.cpp",
      ]
      : [
        "xhcicommandmanager.cpp", "xhcidevice.cpp", "xhciendpoint.cpp",
        "xhcieventmanager.cpp", "xhcimmiospace.cpp", "xhciring.cpp",
        "xhciroothub.cpp", "xhcirootport.cpp", "xhcisharedmemallocator.cpp",
        "xhcislotmanager.cpp", "xhciusbdevice.cpp", "usbaudiocontrol.cpp",
        "usbaudiostreaming.cpp", "usbaudiofunctopology.cpp",
      ]),
    ...(board.rasppi === 5 ? ["usbsubsystem.cpp"] : []),
  ]),
  ...optionalSources(path.join(circleLibBase, "usb"), [
    "usbgamepad8bitdo.cpp", "usbgamepad8bitdopro.cpp", "usbgamepad8bitdoxinput.cpp",
    "usbgamepadxbox360pcwireless.cpp", "usbkeyboard8bitdo.cpp",
  ]),
]);

const soundSources = existingSources(path.join(circleLibBase, "sound"), [
  "soundbasedevice.cpp", "pwmsounddevice.cpp", "hdmisoundbasedevice.cpp",
  "pcm512xsoundcontroller.cpp", "wm8960soundcontroller.cpp",
  ...(board.rasppi === 5
    ? ["i2ssoundbasedevice-rp1.cpp", "pwmsoundbasedevice-rp1.cpp"]
    : ["dmasoundbuffers.cpp", "i2ssoundbasedevice.cpp", "pwmsoundbasedevice.cpp"]),
  ...(board.rasppi >= 4 ? ["usbsoundbasedevice.cpp", "usbsoundcontroller.cpp"] : []),
]);

const fsSources = existingSources(path.join(circleLibBase, "fs"), [
  "partition.cpp", "partitionmanager.cpp",
]);

const fatSources = existingSources(path.join(circleLibBase, "fs", "fat"), [
  "fatfs.cpp", "fatcache.cpp", "fatinfo.cpp", "fat.cpp", "fatdir.cpp",
]);

const inputSources = existingSources(path.join(circleLibBase, "input"), [
  "keyboardbehaviour.cpp", "keymap.cpp", "mousebehaviour.cpp", "mouse.cpp",
  "touchscreen.cpp", "rpitouchscreen.cpp", "xpt2046touchscreen.cpp",
  "console.cpp", "keyboardbuffer.cpp", "linediscipline.cpp",
]);

const ffSources = board.fatfs
  ? existingSources(path.join(config.circleHome, "addon", "fatfs"), [
    "ff.c", "diskio.cpp", "ffsystem.cpp", "ffunicode.c",
  ])
  : [];

const sdCardSources = existingSources(path.join(config.circleHome, "addon", "SDCard"), [
  "emmc.cpp",
  ...(board.rasppi === 5 ? [] : ["mmchost.cpp", "sdhost.cpp"]),
]);

const appBaseSourcesNoInput = [
  "kernel/main.cpp",
  "kernel/kernel.cpp",
  "kernel/memory.cpp",
  "kernel/rom_browser.cpp",
  "libretro/libretro_runner.cpp",
  "platform/circle/circle_log.cpp",
  "platform/circle/circle_timer.cpp",
  "platform/circle/circle_video.cpp",
  "platform/circle/circle_audio.cpp",
  "platform/circle/circle_parallel.cpp",
  "platform/circle/circle_fs.cpp",
];

function appBaseSources() {
  return [
    ...appBaseSourcesNoInput,
    ...(config.noUsb ? [] : ["platform/circle/circle_input.cpp"]),
    ...(config.boardName === "gpi" ? ["platform/circle/circle_gpi.cpp"] : []),
    ...(config.splash ? ["platform/circle/circle_splash.cpp"] : []),
  ];
}

function patternCoreSources() {
  return existingSources(projectRoot, [
    ...appBaseSources(),
    "libretro/builtin_pattern_core.cpp",
  ]);
}

function bootTestSources() {
  return existingSources(projectRoot, [
    "kernel/boottest_main.cpp",
  ]);
}

function bootTestFceummSources() {
  return existingSources(projectRoot, [
    "kernel/boottest_fceumm_main.cpp",
  ]);
}

function fceummSources() {
  const root = path.join(projectRoot, "cores", "libretro-fceumm");
  const core = path.join(root, "src");
  const common = path.join(core, "drivers", "libretro", "libretro-common");

  ensureFile(path.join(root, "Makefile.common"));

  return [
    ...listSources(path.join(core, "boards")),
    ...listSources(path.join(core, "input")),
    ...existingSources(core, [
      "drivers/libretro/libretro.c",
      "drivers/libretro/libretro_dipswitch.c",
      "cart.c",
      "cheat.c",
      "crc32.c",
      "fceu-endian.c",
      "fceu-memory.c",
      "fceu.c",
      "fds.c",
      "fds_apu.c",
      "file.c",
      "filter.c",
      "general.c",
      "input.c",
      "md5.c",
      "nsf.c",
      "palette.c",
      "ppu.c",
      "sound.c",
      "state.c",
      "video.c",
      "vsuni.c",
      "ines.c",
      "unif.c",
      "x6502.c",
    ]),
    path.join(common, "compat", "compat_strl.c"),
    path.join(common, "streams", "memory_stream.c"),
    path.join(projectRoot, "cores", "fceumm_baremetal_compat.c"),
  ];
}

function n64Sources() {
  const root = path.join(projectRoot, "cores", "mupen64plus-libretro-nx");
  const zlib = path.join(root, "custom", "dependencies", "libzlib");

  ensureFile(path.join(root, "Makefile.common"));

  return [
    ...existingSources(root, [
      "custom/mupen64plus-core/api/config.c",
      "mupen64plus-core/src/api/callbacks.c",
      "mupen64plus-core/src/api/debugger.c",
      "mupen64plus-core/src/api/frontend.c",
      "mupen64plus-core/src/backends/api/video_capture_backend.c",
      "mupen64plus-core/src/backends/clock_ctime_plus_delta.c",
      "mupen64plus-core/src/backends/dummy_video_capture.c",
      "mupen64plus-core/src/backends/file_storage.c",
      "mupen64plus-core/src/backends/plugins_compat/audio_plugin_compat.c",
      "mupen64plus-core/src/backends/plugins_compat/input_plugin_compat.c",
      "mupen64plus-core/src/device/cart/af_rtc.c",
      "mupen64plus-core/src/device/cart/cart.c",
      "mupen64plus-core/src/device/cart/cart_rom.c",
      "mupen64plus-core/src/device/cart/eeprom.c",
      "mupen64plus-core/src/device/cart/flashram.c",
      "mupen64plus-core/src/device/cart/is_viewer.c",
      "mupen64plus-core/src/device/cart/sram.c",
      "mupen64plus-core/src/device/controllers/game_controller.c",
      "mupen64plus-core/src/device/controllers/paks/biopak.c",
      "mupen64plus-core/src/device/controllers/paks/mempak.c",
      "mupen64plus-core/src/device/controllers/paks/rumblepak.c",
      "mupen64plus-core/src/device/controllers/paks/transferpak.c",
      "mupen64plus-core/src/device/controllers/vru_controller.c",
      "mupen64plus-core/src/device/dd/dd_controller.c",
      "mupen64plus-core/src/device/dd/disk.c",
      "mupen64plus-core/src/device/device.c",
      "mupen64plus-core/src/device/gb/gb_cart.c",
      "mupen64plus-core/src/device/gb/mbc3_rtc.c",
      "mupen64plus-core/src/device/gb/m64282fp.c",
      "mupen64plus-core/src/device/memory/memory.c",
      "mupen64plus-core/src/device/pif/bootrom_hle.c",
      "mupen64plus-core/src/device/pif/cic.c",
      "mupen64plus-core/src/device/pif/n64_cic_nus_6105.c",
      "mupen64plus-core/src/device/pif/pif.c",
      "mupen64plus-core/src/device/r4300/cached_interp.c",
      "mupen64plus-core/src/device/r4300/cp0.c",
      "mupen64plus-core/src/device/r4300/cp1.c",
      "mupen64plus-core/src/device/r4300/cp2.c",
      "mupen64plus-core/src/device/r4300/idec.c",
      "mupen64plus-core/src/device/r4300/interrupt.c",
      "mupen64plus-core/src/device/r4300/new_dynarec/new_dynarec.c",
      "mupen64plus-core/src/device/r4300/new_dynarec/arm64/linkage_arm64.S",
      "mupen64plus-core/src/device/r4300/pure_interp.c",
      "mupen64plus-core/src/device/r4300/r4300_core.c",
      "mupen64plus-core/src/device/r4300/tlb.c",
      "mupen64plus-core/src/device/rcp/ai/ai_controller.c",
      "mupen64plus-core/src/device/rcp/mi/mi_controller.c",
      "mupen64plus-core/src/device/rcp/pi/pi_controller.c",
      "mupen64plus-core/src/device/rcp/rdp/fb.c",
      "mupen64plus-core/src/device/rcp/rdp/rdp_core.c",
      "mupen64plus-core/src/device/rcp/ri/ri_controller.c",
      "mupen64plus-core/src/device/rcp/rsp/rsp_core.c",
      "mupen64plus-core/src/device/rcp/si/si_controller.c",
      "mupen64plus-core/src/device/rcp/vi/vi_controller.c",
      "mupen64plus-core/src/device/rdram/rdram.c",
      "mupen64plus-core/src/main/cheat.c",
      "mupen64plus-core/src/main/main.c",
      "mupen64plus-core/src/main/rom.c",
      "mupen64plus-core/src/main/savestates.c",
      "mupen64plus-core/src/main/util.c",
      "mupen64plus-core/src/plugin/dummy_audio.c",
      "mupen64plus-core/src/plugin/dummy_input.c",
      "mupen64plus-core/src/plugin/plugin.c",
      "mupen64plus-core/subprojects/md5/md5.c",
      "mupen64plus-core/subprojects/minizip/ioapi.c",
      "mupen64plus-core/subprojects/minizip/unzip.c",
      "mupen64plus-core/subprojects/minizip/zip.c",
      "mupen64plus-rsp-cxd4/rsp.c",
      "mupen64plus-rsp-hle/src/alist.c",
      "mupen64plus-rsp-hle/src/alist_audio.c",
      "mupen64plus-rsp-hle/src/alist_naudio.c",
      "mupen64plus-rsp-hle/src/alist_nead.c",
      "mupen64plus-rsp-hle/src/audio.c",
      "mupen64plus-rsp-hle/src/cicx105.c",
      "mupen64plus-rsp-hle/src/hle.c",
      "mupen64plus-rsp-hle/src/hvqm.c",
      "mupen64plus-rsp-hle/src/jpeg.c",
      "mupen64plus-rsp-hle/src/memory.c",
      "mupen64plus-rsp-hle/src/mp3.c",
      "mupen64plus-rsp-hle/src/musyx.c",
      "mupen64plus-rsp-hle/src/plugin.c",
      "mupen64plus-rsp-hle/src/re2.c",
      "mupen64plus-video-angrylion/interface.c",
      "mupen64plus-video-angrylion/n64video.c",
      "libretro/libretro.c",
      "libretro-common/audio/conversion/float_to_s16.c",
      "libretro-common/audio/conversion/s16_to_float.c",
      "libretro-common/audio/resampler/audio_resampler.c",
      "libretro-common/audio/resampler/drivers/nearest_resampler.c",
      "libretro-common/audio/resampler/drivers/sinc_resampler.c",
      "libretro-common/compat/compat_posix_string.c",
      "libretro-common/compat/compat_strcasestr.c",
      "libretro-common/compat/compat_strl.c",
      "libretro-common/libco/libco.c",
      "libretro-common/memmap/memalign.c",
      "libretro-common/string/stdstring.c",
      "custom/mupen64plus-core/plugin/audio_libretro/audio_backend_libretro.c",
      "custom/mupen64plus-core/plugin/emulate_game_controller_via_libretro.c",
    ]),
    ...listSources(zlib),
    ...existingSources(projectRoot, [
      "cores/n64_baremetal_gl_stubs.c",
      "cores/n64_baremetal_libretro_stubs.c",
      "cores/n64_baremetal_osal_stubs.c",
    ]),
  ];
}

const n64SpeedFlags = [
  "-Ofast",
  "-DNDEBUG",
  "-fomit-frame-pointer",
  "-fno-unwind-tables",
  "-fno-asynchronous-unwind-tables",
  "-fno-stack-protector",
];

// Core bundle (--core=all): several cores in one kernel. Each core is built
// against newlib like on a hosted system, pre-linked into one object, and all
// of its symbols except the prefixed libretro API are made local. That way
// every core keeps its own copy of libretro-common, zlib, globals etc.
const bundleCoreBaseFlags = [
  ...board.cpuFlags,
  ...board.alignFlags,
  "-fsigned-char",
  "-ffunction-sections",
  "-fdata-sections",
  "-w",
  "-I", path.join(projectRoot, "libc", "include"),
];

// GCC 14 turned these C diagnostics into errors; the cores were written for
// compilers that only warned (e.g. int32/uint32 function pointer mismatches).
const legacyCFlags = [
  "-Wno-error=incompatible-pointer-types",
  "-Wno-error=int-conversion",
  "-Wno-error=implicit-function-declaration",
  "-Wno-error=implicit-int",
  "-Wno-error=return-mismatch",
];

function fceummBundleCore() {
  const core = path.join(projectRoot, "cores", "libretro-fceumm", "src");
  const common = path.join(core, "drivers", "libretro", "libretro-common");
  return {
    id: "fceumm",
    sources: fceummSources(),
    includeFirst: [
      "-I", path.join(core, "drivers", "libretro"),
      "-I", path.join(common, "include"),
      "-I", core,
      "-I", path.join(core, "input"),
      "-I", path.join(core, "boards"),
    ],
    cStd: ["-std=gnu99"],
    flags: [
      config.optimize,
      "-D__LIBRETRO__",
      "-DSTATIC_LINKING",
      "-DFRONTEND_SUPPORTS_RGB565",
      "-DHAVE_NO_LANGEXTRA",
      "-DPATH_MAX=1024",
      "-DFCEU_VERSION_NUMERIC=9813",
      "-DGIT_VERSION=\" baremetal\"",
    ],
  };
}

function gambatteBundleCore() {
  const root = path.join(projectRoot, "cores", "gambatte-libretro");
  const lib = path.join(root, "libgambatte");
  const src = path.join(lib, "src");
  ensureFile(path.join(root, "Makefile.common"));
  return {
    id: "gambatte",
    cxx: true,
    sources: [
      ...existingSources(src, [
        "bootloader.cpp", "cpu.cpp", "gambatte.cpp", "gambatte-memory.cpp",
        "initstate.cpp", "interrupter.cpp", "interruptrequester.cpp", "sound.cpp",
        "statesaver.cpp", "tima.cpp", "video.cpp", "video_libretro.cpp",
        "mem/cartridge.cpp", "mem/cartridge_libretro.cpp", "mem/huc3.cpp",
        "mem/memptrs.cpp", "mem/rtc.cpp",
        "sound/channel1.cpp", "sound/channel2.cpp", "sound/channel3.cpp",
        "sound/channel4.cpp", "sound/duty_unit.cpp", "sound/envelope_unit.cpp",
        "sound/length_counter.cpp",
        "video/lyc_irq.cpp", "video/ly_counter.cpp", "video/next_m0_time.cpp",
        "video/ppu.cpp", "video/sprite_mapper.cpp",
      ]),
      ...existingSources(lib, [
        "libretro/libretro.cpp", "libretro/blipper.c", "libretro/cc_resampler.c",
        "libretro/gambatte_log.c",
        "libretro-common/compat/compat_posix_string.c",
        "libretro-common/compat/compat_snprintf.c",
        "libretro-common/compat/compat_strcasestr.c",
        "libretro-common/compat/compat_strl.c",
        "libretro-common/compat/fopen_utf8.c",
        "libretro-common/encodings/encoding_utf.c",
        "libretro-common/file/file_path.c",
        "libretro-common/file/file_path_io.c",
        "libretro-common/streams/file_stream.c",
        "libretro-common/streams/file_stream_transforms.c",
        "libretro-common/string/stdstring.c",
        "libretro-common/time/rtime.c",
        "libretro-common/vfs/vfs_implementation.c",
      ]),
    ],
    includeFirst: [
      "-I", src,
      "-I", path.join(root, "common"),
      "-I", path.join(root, "common", "resample"),
      "-I", path.join(lib, "include"),
      "-I", path.join(lib, "libretro"),
      "-I", path.join(lib, "libretro-common", "include"),
    ],
    cStd: [],
    // Gambatte uses std::string/std::vector and iostreams: libstdc++ headers.
    cxxStd: ["-std=gnu++98", "-fno-exceptions", "-fno-rtti"],
    flags: [
      "-O2",
      "-fomit-frame-pointer",
      "-D__LIBRETRO__",
      "-DHAVE_STDINT_H",
      "-DHAVE_INTTYPES_H",
      "-DNDEBUG",
      "-DVIDEO_RGB565",
      "-DCC_RESAMPLER_NO_HIGHPASS",
    ],
  };
}

function snes9x2002BundleCore() {
  const root = path.join(projectRoot, "cores", "snes9x2002");
  const src = path.join(root, "src");
  ensureFile(path.join(root, "Makefile.common"));
  return {
    id: "snes9x2002",
    sources: [
      ...existingSources(root, [
        "libretro/libretro.c",
        "libretro/libretro-common/streams/memory_stream.c",
      ]),
      // ARM_ASM selects the ARM-tuned renderer (as on the ARM11-based 3DS),
      // the CPU and SPC700 cores stay in C.
      ...existingSources(src, [
        "apu.c", "apuaux.c", "c4.c", "c4emu.c", "cheats.c", "cheats2.c", "clip.c",
        "cpu.c", "cpuexec.c", "cpuops.c", "data.c", "dma.c", "dsp1.c", "fxemu.c",
        "fxinst.c", "globals.c", "memmap.c", "sa1.c", "sa1cpu.c", "sdd1.c",
        "sdd1emu.c", "snapshot.c", "soundux.c", "spc700.c", "srtc.c",
        "ppu.c", "rops.c", "gfx16.c",
        "mode7.c", "mode7new.c", "mode7prio.c", "mode7add.c", "mode7addprio.c",
        "mode7add1_2.c", "mode7add1_2prio.c", "mode7sub.c", "mode7subprio.c",
        "mode7sub1_2.c", "mode7sub1_2prio.c",
        "tile16.c", "tile16add.c", "tile16add1_2.c", "tile16fadd1_2.c",
        "tile16sub.c", "tile16sub1_2.c", "tile16fsub1_2.c",
      ]),
    ],
    includeFirst: [
      "-I", root,
      "-I", path.join(root, "libretro"),
      "-I", path.join(root, "libretro", "libretro-common", "include"),
      "-I", src,
    ],
    cStd: [],
    flags: [
      "-O3",
      "-fomit-frame-pointer",
      "-ffast-math",
      "-finline",
      "-fstrict-aliasing",
      "-D__LIBRETRO__",
      "-DARM_ASM",
      "-DRIGHTSHIFT_IS_SAR",
      "-DHAVE_INTTYPES_H",
      "-DHAVE_STDINT_H",
      "-DHAVE_STRINGS_H",
      "-DLAGFIX",
      "-DUSE_SA1",
      "-DNDEBUG=1",
    ],
  };
}

function picodriveBundleCore() {
  const root = path.join(projectRoot, "cores", "picodrive");
  const lrc = path.join(root, "platform", "libretro", "libretro-common");
  ensureFile(path.join(root, "Makefile.libretro"));
  // Same files as "make -f Makefile.libretro" with the C CPU cores (FAME 68k,
  // CZ80) and without DRCs, ARM assembler and CHD support.
  const tremor = path.join(root, "platform", "common", "tremor");
  const looseAliasing = new Set([
    "pico/draw.c", "pico/draw2.c", "pico/mode4.c", "pico/cd/memory.c", "pico/cd/pcm.c",
  ].map((file) => path.join(root, file)));
  return {
    id: "picodrive",
    sources: existingSources(root, [
      "platform/libretro/libretro.c",
      "platform/libretro/libretro-common/compat/compat_strcasestr.c",
      "platform/libretro/libretro-common/formats/png/rpng.c",
      "platform/libretro/libretro-common/streams/trans_stream.c",
      "platform/libretro/libretro-common/streams/trans_stream_pipe.c",
      "platform/libretro/libretro-common/streams/trans_stream_zlib.c",
      "platform/libretro/libretro-common/file/file_path_io.c",
      "platform/libretro/libretro-common/file/file_path.c",
      "platform/libretro/libretro-common/vfs/vfs_implementation.c",
      "platform/libretro/libretro-common/time/rtime.c",
      "platform/libretro/libretro-common/string/stdstring.c",
      "platform/libretro/libretro-common/encodings/encoding_utf.c",
      "platform/libretro/libretro-common/compat/compat_strl.c",
      "platform/libretro/libretro-common/compat/compat_posix_string.c",
      "platform/libretro/libretro-common/compat/fopen_utf8.c",
      "platform/libretro/libretro-common/streams/file_stream.c",
      "platform/libretro/libretro-common/streams/file_stream_transforms.c",
      "platform/libretro/libretro-common/memmap/memmap.c",
      "platform/common/mp3.c", "platform/common/mp3_sync.c", "platform/common/mp3_drmp3.c",
      "platform/common/ogg.c",
      ...[
        "block.c", "codebook.c", "floor0.c", "floor1.c", "info.c", "mapping0.c", "mdct.c",
        "registry.c", "res012.c", "sharedbook.c", "synthesis.c", "window.c", "vorbisfile.c",
        "framing.c", "bitwise.c",
      ].map((file) => `platform/common/tremor/${file}`),
      ...[
        "gzio.c", "inffast.c", "inflate.c", "inftrees.c", "trees.c", "deflate.c", "crc32.c",
        "adler32.c", "zutil.c", "compress.c", "uncompr.c",
      ].map((file) => `zlib/${file}`),
      "unzip/unzip.c",
      ...[
        "pico.c", "cart.c", "memory.c", "state.c", "sek.c", "z80if.c", "videoport.c", "draw2.c",
        "draw.c", "mode4.c", "misc.c", "eeprom.c", "patch.c", "debug.c", "media.c", "sms.c",
        "cd/mcd.c", "cd/memory.c", "cd/sek.c", "cd/cdc.c", "cd/cdd.c", "cd/cd_image.c",
        "cd/cd_parse.c", "cd/gfx.c", "cd/gfx_dma.c", "cd/misc.c", "cd/pcm.c", "cd/megasd.c",
        "32x/32x.c", "32x/memory.c", "32x/draw.c", "32x/sh2soc.c", "32x/pwm.c",
        "pico/pico.c", "pico/memory.c", "pico/xpcm.c",
        "carthw/carthw.c", "carthw/eeprom_spi.c", "carthw/svp/svp.c", "carthw/svp/memory.c",
        "carthw/svp/ssp16.c",
        "sound/sound.c", "sound/resampler.c", "sound/sn76496.c", "sound/ym2612.c",
        "sound/ym2413.c", "sound/vgm.c", "sound/mix.c",
      ].map((file) => `pico/${file}`),
      "cpu/fame/famec.c",
      "cpu/cz80/cz80.c",
      "cpu/drc/cmn.c",
      "cpu/sh2/sh2.c",
      "cpu/sh2/mame/sh2pico.c",
    ]),
    includeFirst: [
      "-I", root,
      "-I", path.join(lrc, "include"),
      "-I", path.join(lrc, "include", "compat"),
      "-I", path.join(lrc, "include", "encodings"),
      "-I", path.join(lrc, "include", "formats"),
      "-I", path.join(lrc, "include", "streams"),
      "-I", path.join(lrc, "include", "string"),
      "-I", path.join(lrc, "include", "vfs"),
      "-I", path.join(root, "zlib"),
    ],
    cStd: [],
    flags: [
      "-O3",
      "-fomit-frame-pointer",
      "-ffast-math",
      "-falign-functions=2",
      "-fno-common",
      "-D__LIBRETRO__",
      "-DUSE_LIBRETRO_VFS",
      "-DHAVE_ZLIB",
      "-DEMU_F68K",
      "-D_USE_CZ80",
      "-DNDEBUG",
      "-DREVISION=\"-baremetal\"",
    ],
    fileFlags: (source) => {
      if (looseAliasing.has(source)) return ["-fno-strict-aliasing"];
      // CD audio: Tremor (integer Ogg Vorbis decoder).
      if (source.startsWith(tremor) || source === path.join(root, "platform", "common", "ogg.c")) {
        return ["-DUSE_TREMOR", "-I", tremor];
      }
      // famec.c is huge; upstream builds it with reduced optimization.
      if (path.basename(source) === "famec.c") return ["-O2", "-fno-expensive-optimizations"];
      return [];
    },
  };
}

function gpspBundleCore() {
  const root = path.join(projectRoot, "cores", "gpsp");
  ensureFile(path.join(root, "Makefile.common"));
  // "make platform=rpi1": ARM dynarec with an mmap'ed (PROT_EXEC) translation
  // cache, see baremetal/libc for the bare-metal side of that.
  return {
    id: "gpsp",
    cxx: true,
    sources: existingSources(root, [
      "main.c", "cpu.cc", "gba_memory.c", "savestate.c", "video.cc", "input.c",
      "sound.c", "cheats.c", "gbp.c", "serial.c", "serial_proto.c", "rfu.c",
      "gba_cc_lut.c", "memmap.c", "cpu_threaded.c", "bios_data.S", "arm/arm_stub.S",
      "libretro/libretro.c",
      "libretro/libretro-common/compat/compat_posix_string.c",
      "libretro/libretro-common/compat/compat_strl.c",
      "libretro/libretro-common/compat/fopen_utf8.c",
      "libretro/libretro-common/encodings/encoding_utf.c",
      "libretro/libretro-common/file/file_path.c",
      "libretro/libretro-common/file/file_path_io.c",
      "libretro/libretro-common/streams/file_stream.c",
      "libretro/libretro-common/string/stdstring.c",
      "libretro/libretro-common/time/rtime.c",
      "libretro/libretro-common/vfs/vfs_implementation.c",
    ]),
    includeFirst: [
      "-I", root,
      "-I", path.join(root, "libretro"),
      "-I", path.join(root, "libretro", "libretro-common", "include"),
      // Keeps the dynarec's __clear_cache() calls (GCC drops them otherwise).
      "-include", path.join(projectRoot, "libc", "include", "ra_clear_cache.h"),
    ],
    cStd: [],
    cxxStd: ["-std=gnu++11", "-fno-exceptions", "-fno-rtti"],
    flags: [
      "-O3",
      "-fomit-frame-pointer",
      "-ffast-math",
      "-D__LIBRETRO__",
      "-DARM11",
      "-DARM_ARCH",
      "-DHAVE_DYNAREC",
      "-DMMAP_JIT_CACHE",
      "-DFRONTEND_SUPPORTS_RGB565",
      "-DHAVE_STRINGS_H",
      "-DHAVE_STDINT_H",
      "-DHAVE_INTTYPES_H",
      "-DINLINE=inline",
      "-DNDEBUG",
      "-DGIT_VERSION=\" baremetal\"",
    ],
    // bios_data.S embeds the open-source BIOS with a relative .incbin path.
    fileFlags: (source) => (path.basename(source) === "bios_data.S" ? [`-Wa,-I,${root}`] : []),
  };
}

function bundleCores() {
  return [fceummBundleCore(), gambatteBundleCore(), snes9x2002BundleCore(), picodriveBundleCore(), gpspBundleCore()];
}

// Compiles a core, pre-links it into one object and keeps only its libretro
// API global, renamed to <id>_retro_*.
function buildIsolatedCore(core) {
  const objects = core.sources.map((source) => compile(source, core.flags, {
    prependFlags: core.includeFirst,
    baseFlags: bundleCoreBaseFlags,
    cStd: [...(core.cStd || []), ...legacyCFlags],
    cxxStd: core.cxxStd,
    fileFlags: core.fileFlags,
  }));

  const dir = path.join(config.buildDir, "cores");
  ensureDir(dir);
  const merged = path.join(dir, `${core.id}-merged.o`);
  const isolated = path.join(dir, `${core.id}.o`);

  if (newer(merged, objects)) {
    // Response file: the object list exceeds the Windows command line limit.
    const responseFile = `${merged}.rsp`;
    fs.writeFileSync(responseFile, objects.map((object) => `"${object.replace(/\\/g, "/")}"`).join("\n"));
    run("LD -r", tools.ld, ["-r", "-o", merged, `@${responseFile}`], { display: relFromRoot(merged) });
  }

  if (newer(isolated, [merged])) {
    const redefine = `${isolated}.redefine`;
    const keep = `${isolated}.keep`;
    fs.writeFileSync(redefine, libretroApiSymbols.map((symbol) => `${symbol} ${core.id}_${symbol}`).join("\n") + "\n");
    fs.writeFileSync(keep, libretroApiSymbols.map((symbol) => `${core.id}_${symbol}`).join("\n") + "\n");
    run("ISOLATE", tools.objcopy, [
      `--redefine-syms=${redefine}`,
      `--keep-global-symbols=${keep}`,
      merged,
      isolated,
    ], { display: relFromRoot(isolated) });
  }

  return isolated;
}

function buildCoreArchive() {
  if (config.core === "all") {
    const cores = bundleCores();
    return {
      appSources: existingSources(projectRoot, [...appBaseSources(), "libc/newlib_glue.cpp", "libc/circle_bridge.cpp"]),
      archives: [],
      objects: cores.map(buildIsolatedCore),
      extraLibs: ["libc", ...(cores.some((core) => core.cxx) ? ["libstdc++"] : [])],
    };
  }

  if (config.core === "pattern") {
    return { appSources: patternCoreSources(), archives: [], extraLibs: [] };
  }

  if (config.core === "boottest") {
    return { appSources: bootTestSources(), archives: [], extraLibs: [] };
  }

  if (config.core === "boottest-fceumm") {
    const root = path.join(projectRoot, "cores", "libretro-fceumm");
    const core = path.join(root, "src");
    const common = path.join(core, "drivers", "libretro", "libretro-common");
    const includeFirst = [
      "-I", path.join(core, "drivers", "libretro"),
      "-I", path.join(common, "include"),
      "-I", core,
      "-I", path.join(core, "input"),
      "-I", path.join(core, "boards"),
    ];
    const flags = [
      "-D__LIBRETRO__",
      "-DSTATIC_LINKING",
      "-DFRONTEND_SUPPORTS_RGB565",
      "-DHAVE_NO_LANGEXTRA",
      "-DPATH_MAX=1024",
      "-DFCEU_VERSION_NUMERIC=9813",
      "-DGIT_VERSION=\" baremetal\"",
      formatDefine("PRId64"),
      formatDefine("PRIu64"),
      formatDefine("PRIuPTR"),
      "-ffunction-sections",
      "-fdata-sections",
      "-Wno-unused-function",
      "-Wno-unused-variable",
      "-Wno-missing-braces",
      "-Wno-implicit-fallthrough",
    ];

    const archiveFile = archive("libfceumm.a", fceummSources(), flags, { prependFlags: includeFirst });
    return { appSources: bootTestFceummSources(), archives: [archiveFile], extraLibs: [] };
  }

  if (config.core === "multi") {
    const fceummRoot = path.join(projectRoot, "cores", "libretro-fceumm");
    const fceummCore = path.join(fceummRoot, "src");
    const fceummCommon = path.join(fceummCore, "drivers", "libretro", "libretro-common");
    const fceummIncludeFirst = [
      "-I", path.join(fceummCore, "drivers", "libretro"),
      "-I", path.join(fceummCommon, "include"),
      "-I", fceummCore,
      "-I", path.join(fceummCore, "input"),
      "-I", path.join(fceummCore, "boards"),
    ];
    const fceummFlags = [
      ...libretroSymbolDefines("fceumm"),
      ...prefixedGlobalDefines("fceumm", [
        "environ_cb",
        ...multiCoreSharedSymbolDefines,
      ]),
      "-D__LIBRETRO__",
      "-DSTATIC_LINKING",
      "-DFRONTEND_SUPPORTS_RGB565",
      "-DHAVE_NO_LANGEXTRA",
      "-DPATH_MAX=1024",
      "-DFCEU_VERSION_NUMERIC=9813",
      "-DGIT_VERSION=\" baremetal\"",
      formatDefine("PRId64"),
      formatDefine("PRIu64"),
      formatDefine("PRIuPTR"),
      "-ffunction-sections",
      "-fdata-sections",
      "-Wno-unused-function",
      "-Wno-unused-variable",
      "-Wno-missing-braces",
      "-Wno-implicit-fallthrough",
    ];

    const n64Root = path.join(projectRoot, "cores", "mupen64plus-libretro-nx");
    const n64IncludeFirst = [
      "-I", path.join(projectRoot, "cores", "n64_compat"),
      "-I", path.join(n64Root, "libretro"),
      "-I", path.join(n64Root, "custom"),
      "-I", path.join(n64Root, "custom", "mupen64plus-core"),
      "-I", path.join(n64Root, "custom", "mupen64plus-core", "api"),
      "-I", path.join(n64Root, "custom", "mupen64plus-core", "plugin", "audio_libretro"),
      "-I", path.join(n64Root, "custom", "android", "include"),
      "-I", path.join(n64Root, "custom", "GLideN64"),
      "-I", path.join(n64Root, "custom", "dependencies", "libzlib"),
      "-I", path.join(n64Root, "mupen64plus-core", "src"),
      "-I", path.join(n64Root, "mupen64plus-core", "src", "api"),
      "-I", path.join(n64Root, "mupen64plus-core", "src", "main"),
      "-I", path.join(n64Root, "mupen64plus-core", "src", "osal"),
      "-I", path.join(n64Root, "mupen64plus-core", "src", "plugin"),
      "-I", path.join(n64Root, "mupen64plus-core", "src", "asm_defines"),
      "-I", path.join(n64Root, "mupen64plus-core", "src", "device", "r4300", "new_dynarec", "arm64"),
      "-I", path.join(n64Root, "mupen64plus-core", "subprojects", "md5"),
      "-I", path.join(n64Root, "mupen64plus-core", "subprojects", "minizip"),
      "-I", path.join(n64Root, "mupen64plus-rsp-cxd4"),
      "-I", path.join(n64Root, "mupen64plus-video-angrylion"),
      "-I", path.join(n64Root, "mupen64plus-video-angrylion", "n64video"),
      "-I", path.join(n64Root, "libretro-common", "include"),
      "-I", path.join(n64Root, "GLideN64", "src", "inc"),
      "-I", path.join(n64Root, "GLideN64", "src", "osal"),
      "-I", path.join(n64Root, "xxHash"),
    ];
    const n64Flags = [
      ...libretroSymbolDefines("n64"),
      ...prefixedGlobalDefines("n64", [
        "audio_batch_cb",
        "environ_cb",
        "environ_clear_thread_waits_cb",
        "input_cb",
        "log_cb",
        "perf_cb",
        "poll_cb",
        "retro_screen_aspect",
        "retro_screen_height",
        "retro_screen_width",
        "rumble",
        "video_cb",
        ...multiCoreSharedSymbolDefines,
      ]),
      "-std=gnu11",
      "-D__LIBRETRO__",
      "-DSTATIC_LINKING",
      "-DM64P_PLUGIN_API",
      "-DM64P_CORE_PROTOTYPES",
      "-DDYNAREC",
      "-DNEW_DYNAREC=4",
      "-D_ENDUSER_RELEASE",
      "-D__STDC_CONSTANT_MACROS",
      "-D__STDC_LIMIT_MACROS",
      "-D__STDC_FORMAT_MACROS",
      "-DUSE_FILE32API",
      "-DSINC_LOWER_QUALITY",
      "-DTXFILTER_LIB",
      "-D__VEC4_OPT",
      "-DMUPENPLUSAPI",
      "-DHAVE_THR_AL",
      "-DHAVE_LLE",
      "-DCORE_NAME=\"mupen64plus\"",
      "-DGIT_VERSION=\" baremetal\"",
      "-DPATH_MAX=1024",
      formatDefine("PRIX64"),
      "-DPRIX32=\"X\"",
      "-DPRIX16=\"X\"",
      "-DPRIX8=\"X\"",
      formatDefine("PRIxPTR"),
      formatDefine("PRIuPTR"),
      formatDefine("PRIu64"),
      formatDefine("PRId64"),
      "-D_CRT_SECURE_NO_WARNINGS",
      ...n64SpeedFlags,
      "-Wno-unused-function",
      "-Wno-unused-variable",
      "-Wno-missing-braces",
      "-Wno-implicit-fallthrough",
      "-Wno-discarded-qualifiers",
      "-Wno-unknown-pragmas",
      "-ffunction-sections",
      "-fdata-sections",
    ];

    const fceummArchive = archive("libfceumm_multi.a", fceummSources(), fceummFlags, { prependFlags: fceummIncludeFirst });
    const n64Archive = archive("libmupen64plus_next_multi.a", n64Sources(), n64Flags, { prependFlags: n64IncludeFirst });
    return {
      appSources: existingSources(projectRoot, appBaseSources()),
      archives: [fceummArchive, n64Archive],
      extraLibs: ["libc"],
    };
  }

  if (config.core === "n64") {
    const root = path.join(projectRoot, "cores", "mupen64plus-libretro-nx");
    const includeFirst = [
      "-I", path.join(projectRoot, "cores", "n64_compat"),
      "-I", path.join(root, "libretro"),
      "-I", path.join(root, "custom"),
      "-I", path.join(root, "custom", "mupen64plus-core"),
      "-I", path.join(root, "custom", "mupen64plus-core", "api"),
      "-I", path.join(root, "custom", "mupen64plus-core", "plugin", "audio_libretro"),
      "-I", path.join(root, "custom", "android", "include"),
      "-I", path.join(root, "custom", "GLideN64"),
      "-I", path.join(root, "custom", "dependencies", "libzlib"),
      "-I", path.join(root, "mupen64plus-core", "src"),
      "-I", path.join(root, "mupen64plus-core", "src", "api"),
      "-I", path.join(root, "mupen64plus-core", "src", "main"),
      "-I", path.join(root, "mupen64plus-core", "src", "osal"),
      "-I", path.join(root, "mupen64plus-core", "src", "plugin"),
      "-I", path.join(root, "mupen64plus-core", "src", "asm_defines"),
      "-I", path.join(root, "mupen64plus-core", "src", "device", "r4300", "new_dynarec", "arm64"),
      "-I", path.join(root, "mupen64plus-core", "subprojects", "md5"),
      "-I", path.join(root, "mupen64plus-core", "subprojects", "minizip"),
      "-I", path.join(root, "mupen64plus-rsp-cxd4"),
      "-I", path.join(root, "mupen64plus-video-angrylion"),
      "-I", path.join(root, "mupen64plus-video-angrylion", "n64video"),
      "-I", path.join(root, "libretro-common", "include"),
      "-I", path.join(root, "GLideN64", "src", "inc"),
      "-I", path.join(root, "GLideN64", "src", "osal"),
      "-I", path.join(root, "xxHash"),
    ];
    const flags = [
      "-std=gnu11",
      "-D__LIBRETRO__",
      "-DSTATIC_LINKING",
      "-DM64P_PLUGIN_API",
      "-DM64P_CORE_PROTOTYPES",
      "-DDYNAREC",
      "-DNEW_DYNAREC=4",
      "-D_ENDUSER_RELEASE",
      "-D__STDC_CONSTANT_MACROS",
      "-D__STDC_LIMIT_MACROS",
      "-D__STDC_FORMAT_MACROS",
      "-DUSE_FILE32API",
      "-DSINC_LOWER_QUALITY",
      "-DTXFILTER_LIB",
      "-D__VEC4_OPT",
      "-DMUPENPLUSAPI",
      "-DHAVE_THR_AL",
      "-DHAVE_LLE",
      "-DCORE_NAME=\"mupen64plus\"",
      "-DGIT_VERSION=\" baremetal\"",
      "-DPATH_MAX=1024",
      formatDefine("PRIX64"),
      "-DPRIX32=\"X\"",
      "-DPRIX16=\"X\"",
      "-DPRIX8=\"X\"",
      formatDefine("PRIxPTR"),
      formatDefine("PRIuPTR"),
      formatDefine("PRIu64"),
      formatDefine("PRId64"),
      "-D_CRT_SECURE_NO_WARNINGS",
      ...n64SpeedFlags,
      "-Wno-unused-function",
      "-Wno-unused-variable",
      "-Wno-missing-braces",
      "-Wno-implicit-fallthrough",
      "-Wno-discarded-qualifiers",
      "-Wno-unknown-pragmas",
      "-ffunction-sections",
      "-fdata-sections",
    ];

    const archiveFile = archive("libmupen64plus_next_baremetal.a", n64Sources(), flags, { prependFlags: includeFirst });
    return {
      appSources: existingSources(projectRoot, appBaseSources()),
      archives: [archiveFile],
      extraLibs: ["libc"],
    };
  }

  if (config.core !== "fceumm") {
    fail(`Unknown core '${config.core}'. Use 'pattern', 'boottest', 'boottest-fceumm', 'fceumm', 'n64' or 'multi'.`);
  }

  const root = path.join(projectRoot, "cores", "libretro-fceumm");
  const core = path.join(root, "src");
  const common = path.join(core, "drivers", "libretro", "libretro-common");
  const includeFirst = [
    "-I", path.join(core, "drivers", "libretro"),
    "-I", path.join(common, "include"),
    "-I", core,
    "-I", path.join(core, "input"),
    "-I", path.join(core, "boards"),
  ];
  const flags = [
    "-D__LIBRETRO__",
    "-DSTATIC_LINKING",
    "-DFRONTEND_SUPPORTS_RGB565",
    "-DHAVE_NO_LANGEXTRA",
    "-DPATH_MAX=1024",
    "-DFCEU_VERSION_NUMERIC=9813",
    "-DGIT_VERSION=\" baremetal\"",
    formatDefine("PRId64"),
    formatDefine("PRIu64"),
    formatDefine("PRIuPTR"),
    "-ffunction-sections",
    "-fdata-sections",
    "-Wno-unused-function",
    "-Wno-unused-variable",
    "-Wno-missing-braces",
    "-Wno-implicit-fallthrough",
  ];

  const archiveFile = archive("libfceumm.a", fceummSources(), flags, { prependFlags: includeFirst });
  return {
    appSources: existingSources(projectRoot, appBaseSources()),
    archives: [archiveFile],
    extraLibs: [],
  };
}

function generateSplash() {
  const output = path.join(config.buildDir, "gen", "splash_image.c");
  const { width, height } = writeSplashSource(config.splash, board.screen.width, board.screen.height, output);
  console.log(`SPLASH  ${relFromRoot(config.splash)} -> ${width}x${height} RGB565`);
  return output;
}

async function main() {
  for (const file of Object.values(tools)) ensureFile(file);
  ensureFile(path.join(config.circleHome, "circle.ld"));
  ensureDir(config.buildDir);

  console.log(`Node build for ${board.label} Circle image`);
  console.log(`Board:    ${config.boardName} (RASPPI=${board.rasppi}, AARCH=${board.aarch})`);
  console.log(`Circle:   ${config.circleHome} (${config.circleVersion})`);
  console.log(`Toolchain:${config.toolchain}`);
  console.log(`Core:     ${config.core}`);
  console.log(`ROM:      ${config.romPath}`);
  console.log(`USB:      ${config.noUsb ? "disabled" : "enabled"}`);
  console.log(`Kernel max size: ${config.kernelMaxSize}`);
  if (config.core === "n64" || config.core === "multi") {
    console.log(`N64 skip: ${config.n64FrameSkip} frames; present divisor ${config.n64ViDivisor}`);
  }
  console.log(`Splash:   ${config.splash ? relFromRoot(config.splash) : "disabled (text boot log)"}`);
  console.log(`Out:      ${config.buildDir}`);

  const coreBuild = buildCoreArchive();
  const libcircle = archive("libcircle.a", circleCoreSources, ["-DNO_SANITIZE=1"]);
  const libusb = archive("libusb.a", usbSources);
  const libsound = archive("libsound.a", soundSources);
  const libfs = archive("libfs.a", fsSources);
  const libfatfs = archive("libfatfs.a", fatSources);
  const libinput = archive("libinput.a", inputSources);
  const libsdcard = archive("libsdcard.a", sdCardSources);
  const libff = board.fatfs ? [archive("libff.a", ffSources)] : [];
  const usesSplash = config.splash && coreBuild.appSources.some((source) => path.basename(source) === "circle_splash.cpp");
  const generatedSources = usesSplash ? [generateSplash()] : [];
  const appObjects = [...coreBuild.appSources, ...generatedSources].map((source) => compile(source));
  const { libgcc, libm, libc, libnosys, libstdcxx } = parseLibs();
  const coreArchives = coreBuild.archives;
  const coreObjects = coreBuild.objects || [];
  const extraLibs = [
    ...(coreBuild.extraLibs.includes("libstdc++") ? [libstdcxx] : []),
    ...(coreBuild.extraLibs.includes("libc") ? [libc, libnosys] : []),
  ];
  // Circle and newlib/libstdc++ both define some C runtime functions
  // (malloc, memcpy, setjmp, __cxa_guard_*); Circle's come first and win.
  const allowMultipleDefinition = config.core === "n64" || config.core === "multi" || config.core === "all";

  const ldHelp = spawnSync(tools.ld, ["--help"], { encoding: "utf8" });
  const noWarnRwx = `${ldHelp.stdout || ""}${ldHelp.stderr || ""}`.includes("no-warn-rwx-segments")
    ? ["--no-warn-rwx-segments"]
    : [];
  const gcSections = config.core === "fceumm" || config.core === "all" || config.core === "n64" || config.core === "multi" || config.core === "boottest" || config.core === "boottest-fceumm" ? ["--gc-sections"] : [];

  const elf = path.join(config.buildDir, `${config.target}.elf`);
  const map = path.join(config.buildDir, `${config.target}.map`);
  const img = path.join(config.buildDir, `${config.target}.img`);
  const lst = path.join(config.buildDir, `${config.target}.lst`);
  const allLinkInputs = [
    ...appObjects,
    ...coreObjects,
    ...coreArchives,
    ...libff,
    libsdcard, libsound, libusb, libinput, libfatfs, libfs, libcircle, libgcc, libm, ...extraLibs,
    path.join(config.circleHome, "circle.ld"),
  ];

  if (newer(elf, allLinkInputs)) {
    run("LD", tools.ld, [
      "-o", elf,
      "-Map", map,
      `--section-start=.init=${board.loadAddress}`,
      ...noWarnRwx,
      ...(allowMultipleDefinition ? ["--allow-multiple-definition"] : []),
      ...gcSections,
      "-T", path.join(config.circleHome, "circle.ld"),
      ...appObjects,
      ...coreObjects,
      "--start-group",
      ...coreArchives,
      ...libff,
      libsdcard, libsound, libusb, libinput, libfatfs, libfs, libcircle, libgcc, libm, ...extraLibs,
      "--end-group",
    ], { display: relFromRoot(elf) });
  }

  if (newer(img, [elf])) {
    run("COPY", tools.objcopy, [elf, "-O", "binary", img], { display: relFromRoot(img) });
  }

  const dump = spawnSync(tools.objdump, ["-d", elf], { encoding: "utf8", maxBuffer: 256 * 1024 * 1024 });
  if (dump.status === 0) {
    const filt = spawnSync(tools.cxxfilt, [], { input: dump.stdout, encoding: "utf8", maxBuffer: 256 * 1024 * 1024 });
    if (filt.status === 0) {
      fs.writeFileSync(lst, filt.stdout);
      console.log(`DUMP    ${relFromRoot(lst)}`);
    }
  }

  const size = fs.statSync(img).size;
  console.log(`\nBuilt ${img}`);
  console.log(`Size: ${size.toLocaleString("en-US")} bytes`);
  // Circle places the kernel stack right after KERNEL_MAX_SIZE, so .bss must
  // end before that too, not just the image file.
  const symbols = run("QUERY", tools.objdump, ["-t", elf], { capture: true });
  const endMatch = symbols.match(/^([0-9a-f]+)\s.*\s_end$/m);
  const kernelEnd = endMatch ? Number.parseInt(endMatch[1], 16) : Number(board.loadAddress) + size;
  const kernelUsed = kernelEnd - Number(board.loadAddress);
  console.log(`Memory: ${kernelUsed.toLocaleString("en-US")} bytes incl. .bss (limit ${Number(config.kernelMaxSize).toLocaleString("en-US")})`);
  if (kernelUsed > Number(config.kernelMaxSize)) {
    fail(`Kernel incl. .bss exceeds KERNEL_MAX_SIZE (${config.kernelMaxSize}); pass --kernel-max-size=<bytes>.`);
  }

  if (config.sdCard) {
    await assembleSdCard({
      ...board.sdCard,
      kernelImage: img,
      kernelName: `${config.target}.img`,
      circleHome: config.circleHome,
    });
  } else {
    console.log(`Copy it to the ${board.label} boot partition as ${config.target}.img.`);
  }
}

await main();
