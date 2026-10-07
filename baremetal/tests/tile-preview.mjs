#!/usr/bin/env node
// Renders the tile ROM browser (kernel/tile_view.cpp) on the PC with the real
// system pictures and Circle's fonts, as PNG files in
// baremetal/build-node/host-tests/previews/. A cover picture stands for a
// <rom name>.png on the SD card (default: assets/splash.png).
//
// usage: node baremetal/tests/tile-preview.mjs [cover.png]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

import { writeSystemImagesSource } from "../splash.mjs";

const testsDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(testsDir, "..");
const circleHome = path.resolve(projectRoot, "..", "circle");
const outDir = path.join(projectRoot, "build-node", "host-tests");
const previewDir = path.join(outDir, "previews");
fs.mkdirSync(previewDir, { recursive: true });

function run(exe, args) {
  const result = spawnSync(exe, args, { stdio: ["ignore", "pipe", "inherit"], encoding: "utf8" });
  if (result.status !== 0) {
    console.error(result.stdout);
    process.exit(1);
  }
  return result.stdout;
}

const systemImages = path.join(outDir, "system_images.c");
writeSystemImagesSource(path.join(projectRoot, "assets", "systems"), systemImages);

const cc = process.env.CC || "cc";
const cxx = process.env.CXX || "c++";
const include = ["-I", projectRoot, "-I", path.join(circleHome, "include")];
const objects = [];
for (const [compiler, source, flags] of [
  [cc, systemImages, ["-std=c99"]],
  [cxx, path.join(circleHome, "lib", "font6x7.cpp"), ["-std=c++17"]],
  [cxx, path.join(circleHome, "lib", "font8x8.cpp"), ["-std=c++17"]],
  [cxx, path.join(circleHome, "lib", "font8x16.cpp"), ["-std=c++17"]],
  [cxx, path.join(circleHome, "lib", "font12x22.cpp"), ["-std=c++17"]],
  [cxx, path.join(projectRoot, "kernel", "tile_view.cpp"), ["-std=c++17", "-Wall", "-Wextra"]],
  [cxx, path.join(testsDir, "tile_preview.cpp"), ["-std=c++17", "-Wall"]],
]) {
  const object = path.join(outDir, `${path.basename(source)}.o`);
  run(compiler, [...flags, "-O2", ...include, "-c", "-o", object, source]);
  objects.push(object);
}
const binary = path.join(outDir, "tile_preview");
run(cxx, ["-o", binary, ...objects]);

const cover = path.resolve(process.argv[2] || path.join(projectRoot, "assets", "splash.png"));
const ppmFiles = run(binary, [previewDir, cover]).trim().split("\n");

// PPM (P6) to PNG, without extra packages.
const crcTable = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function crc32(buffer) {
  let c = 0xffffffff;
  for (const byte of buffer) c = crcTable[(c ^ byte) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const out = Buffer.alloc(12 + data.length);
  out.writeUInt32BE(data.length, 0);
  out.write(type, 4, "latin1");
  data.copy(out, 8);
  out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
  return out;
}
for (const ppm of ppmFiles) {
  const data = fs.readFileSync(ppm);
  const header = data.toString("latin1", 0, 32).match(/^P6\s+(\d+)\s+(\d+)\s+255\s/);
  const [width, height] = [Number(header[1]), Number(header[2])];
  const pixels = data.subarray(header[0].length);
  const raw = Buffer.alloc((width * 3 + 1) * height);
  for (let y = 0; y < height; y++) pixels.copy(raw, y * (width * 3 + 1) + 1, y * width * 3, (y + 1) * width * 3);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr.set([8, 2, 0, 0, 0], 8);
  const png = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk("IHDR", ihdr), chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0)),
  ]);
  const file = ppm.replace(/\.ppm$/, ".png");
  fs.writeFileSync(file, png);
  fs.rmSync(ppm);
  console.log(path.relative(process.cwd(), file));
}
