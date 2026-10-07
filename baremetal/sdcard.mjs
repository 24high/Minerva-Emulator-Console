// Assembles the boot files for an SD card and, when dosfstools and mtools
// are installed, a flashable disk image with one FAT32 partition.
import { spawnSync } from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(projectRoot, "..");

const SECTOR_SIZE = 512;
const PARTITION_START_SECTOR = 2048; // 1 MiB alignment, as used by Raspberry Pi OS images
const DEFAULT_IMAGE_MB = 256;

function rel(file) {
  return path.relative(repoRoot, file).replace(/\\/g, "/");
}

// Use the firmware revision Circle has been tested with (circle/boot/Makefile).
function firmwareRevision(circleHome) {
  const makefile = path.join(circleHome, "boot", "Makefile");
  const match = fs.existsSync(makefile)
    ? fs.readFileSync(makefile, "utf8").match(/^FIRMWARE\s*\?=\s*([0-9a-f]{40})/m)
    : null;
  return process.env.FIRMWARE || (match ? match[1] : "master");
}

async function fetchFirmware(files, circleHome) {
  const revision = firmwareRevision(circleHome);
  const cacheDir = path.join(projectRoot, "firmware-cache", revision);
  fs.mkdirSync(cacheDir, { recursive: true });

  for (const file of files) {
    const target = path.join(cacheDir, file);
    if (fs.existsSync(target) && fs.statSync(target).size > 0) continue;

    const url = `https://github.com/raspberrypi/firmware/raw/${revision}/boot/${file}`;
    console.log(`FETCH   ${url}`);
    const response = await fetch(url);
    if (!response.ok) {
      throw new Error(`Download failed (${response.status}): ${url}`);
    }
    fs.writeFileSync(`${target}.part`, Buffer.from(await response.arrayBuffer()));
    fs.renameSync(`${target}.part`, target);
  }

  return cacheDir;
}

function findTool(names) {
  const dirs = [...(process.env.PATH || "").split(path.delimiter), "/usr/sbin", "/sbin"];
  for (const name of names) {
    for (const dir of dirs) {
      const candidate = path.join(dir, name);
      if (dir && fs.existsSync(candidate)) return candidate;
    }
  }
  return null;
}

function runTool(exe, args) {
  const result = spawnSync(exe, args, { encoding: "utf8" });
  if (result.status !== 0) {
    throw new Error(`${path.basename(exe)} failed: ${result.stderr || result.stdout}`);
  }
}

function writeMbr(image, totalSectors) {
  const mbr = Buffer.alloc(SECTOR_SIZE);
  crypto.randomBytes(4).copy(mbr, 440); // disk signature
  const entry = 446;
  mbr[entry + 0] = 0x00; // not bootable; the Pi firmware does not care
  mbr.writeUInt8(0xfe, entry + 1); // CHS start: unused, LBA addressing
  mbr.writeUInt16LE(0xffff, entry + 2);
  mbr[entry + 4] = 0x0c; // FAT32 (LBA)
  mbr.writeUInt8(0xfe, entry + 5);
  mbr.writeUInt16LE(0xffff, entry + 6);
  mbr.writeUInt32LE(PARTITION_START_SECTOR, entry + 8);
  mbr.writeUInt32LE(totalSectors - PARTITION_START_SECTOR, entry + 12);
  mbr[510] = 0x55;
  mbr[511] = 0xaa;

  const fd = fs.openSync(image, "r+");
  try {
    fs.writeSync(fd, mbr, 0, SECTOR_SIZE, 0);
  } finally {
    fs.closeSync(fd);
  }
}

export function buildImage(image, sourceDir) {
  const mkfs = findTool(["mkfs.vfat", "mkfs.fat"]);
  const mcopy = findTool(["mcopy"]);
  if (!mkfs || !mcopy) {
    console.log("SKIP    disk image: install dosfstools (mkfs.vfat) and mtools (mcopy) to create it.");
    return false;
  }

  const imageMb = Number.parseInt(process.env.SD_IMAGE_MB || "", 10) || DEFAULT_IMAGE_MB;
  const totalSectors = (imageMb * 1024 * 1024) / SECTOR_SIZE;
  const partitionKiB = ((totalSectors - PARTITION_START_SECTOR) * SECTOR_SIZE) / 1024;

  fs.mkdirSync(path.dirname(image), { recursive: true });
  fs.rmSync(image, { force: true });
  fs.writeFileSync(image, "");
  fs.truncateSync(image, totalSectors * SECTOR_SIZE);
  writeMbr(image, totalSectors);

  console.log(`MKFS    ${rel(image)} (${imageMb} MiB, FAT32)`);
  runTool(mkfs, ["-F", "32", "-S", String(SECTOR_SIZE), "-n", "MINERVA", "--offset", String(PARTITION_START_SECTOR), image, String(partitionKiB)]);

  const target = `${image}@@${PARTITION_START_SECTOR * SECTOR_SIZE}`;
  const entries = fs.readdirSync(sourceDir).map((name) => path.join(sourceDir, name));
  runTool(mcopy, ["-i", target, "-s", "-o", "-Q", ...entries, "::/"]);
  return true;
}

export async function assembleSdCard(options) {
  const { configDir, outputDir, image, firmware, kernelImage, kernelName, circleHome } = options;

  const firmwareDir = await fetchFirmware(firmware, circleHome);

  // Only the managed boot files are overwritten, so ROMs copied into the
  // output folder survive a rebuild.
  fs.mkdirSync(outputDir, { recursive: true });
  for (const file of firmware) {
    fs.copyFileSync(path.join(firmwareDir, file), path.join(outputDir, file));
  }
  for (const file of fs.readdirSync(configDir)) {
    fs.copyFileSync(path.join(configDir, file), path.join(outputDir, file));
  }
  fs.copyFileSync(kernelImage, path.join(outputDir, kernelName));
  console.log(`SDCARD  ${rel(outputDir)}/`);

  const imageBuilt = buildImage(image, outputDir);

  console.log("\nSD card:");
  console.log(`  Files: copy the contents of ${rel(outputDir)}/ to a FAT32-formatted SD card.`);
  if (imageBuilt) {
    console.log(`  Image: flash ${rel(image)} (e.g. with Raspberry Pi Imager or dd).`);
  }
  console.log("  ROMs:  put the game files into the root or any folder of the FAT partition.");
}
