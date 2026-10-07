// Converts the boot image (a PNG) into an RGB565 C array that the kernel
// shows while booting. Uses only Node's zlib, so no image packages are needed.
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
const CHANNELS = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// Decodes a non-interlaced PNG of any colour type and bit depth into RGBA8.
export function decodePng(file) {
  const data = fs.readFileSync(file);
  if (!data.subarray(0, 8).equals(PNG_SIGNATURE)) {
    throw new Error(`${file}: not a PNG file`);
  }

  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  let interlace = 0;
  let palette = null;
  let transparency = null;
  const idat = [];
  for (let offset = 8; offset + 8 <= data.length;) {
    const length = data.readUInt32BE(offset);
    const type = data.toString("latin1", offset + 4, offset + 8);
    const chunk = data.subarray(offset + 8, offset + 8 + length);
    offset += 12 + length;

    if (type === "IHDR") {
      width = chunk.readUInt32BE(0);
      height = chunk.readUInt32BE(4);
      bitDepth = chunk[8];
      colorType = chunk[9];
      interlace = chunk[12];
    } else if (type === "PLTE") {
      palette = chunk;
    } else if (type === "tRNS") {
      transparency = chunk;
    } else if (type === "IDAT") {
      idat.push(chunk);
    } else if (type === "IEND") {
      break;
    }
  }

  const channels = CHANNELS[colorType];
  if (!channels) throw new Error(`${file}: unsupported PNG colour type ${colorType}`);
  if (interlace !== 0) throw new Error(`${file}: interlaced PNGs are not supported, save it without interlacing`);
  if (colorType === 3 && !palette) throw new Error(`${file}: palette PNG without PLTE chunk`);

  const bitsPerPixel = channels * bitDepth;
  const stride = Math.ceil((width * bitsPerPixel) / 8);
  const filterDistance = Math.max(1, bitsPerPixel >> 3);
  const raw = zlib.inflateSync(Buffer.concat(idat));

  const pixels = Buffer.alloc(stride * height);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const line = pixels.subarray(y * stride, (y + 1) * stride);
    const prev = y > 0 ? pixels.subarray((y - 1) * stride, y * stride) : null;
    for (let x = 0; x < stride; x++) {
      const a = x >= filterDistance ? line[x - filterDistance] : 0;
      const b = prev ? prev[x] : 0;
      const c = prev && x >= filterDistance ? prev[x - filterDistance] : 0;
      let value = src[x];
      switch (filter) {
        case 0: break;
        case 1: value += a; break;
        case 2: value += b; break;
        case 3: value += (a + b) >> 1; break;
        case 4: value += paeth(a, b, c); break;
        default: throw new Error(`${file}: invalid PNG filter type ${filter}`);
      }
      line[x] = value & 0xff;
    }
  }

  // Sample `index` of a scanline; palette indices stay raw, other samples
  // are scaled to 0-255.
  const sample = (line, index) => {
    if (bitDepth === 8) return line[index];
    if (bitDepth === 16) return line[index * 2];
    const bitOffset = index * bitDepth;
    const max = (1 << bitDepth) - 1;
    const value = (line[bitOffset >> 3] >> (8 - bitDepth - (bitOffset & 7))) & max;
    return colorType === 3 ? value : Math.round((value * 255) / max);
  };

  const rgba = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y++) {
    const line = pixels.subarray(y * stride, (y + 1) * stride);
    for (let x = 0; x < width; x++) {
      let r;
      let g;
      let b;
      let alpha = 255;
      if (colorType === 3) {
        const index = sample(line, x);
        r = palette[index * 3];
        g = palette[index * 3 + 1];
        b = palette[index * 3 + 2];
        if (transparency && index < transparency.length) alpha = transparency[index];
      } else if (colorType === 0 || colorType === 4) {
        r = g = b = sample(line, x * channels);
        if (colorType === 4) alpha = sample(line, x * channels + 1);
      } else {
        r = sample(line, x * channels);
        g = sample(line, x * channels + 1);
        b = sample(line, x * channels + 2);
        if (colorType === 6) alpha = sample(line, x * channels + 3);
      }
      rgba.set([r, g, b, alpha], (y * width + x) * 4);
    }
  }

  return { width, height, rgba };
}

// Box-filter downscale; transparent pixels are composited onto black.
function fitToScreen(image, maxWidth, maxHeight) {
  const scale = Math.min(1, maxWidth / image.width, maxHeight / image.height);
  const width = Math.max(1, Math.round(image.width * scale));
  const height = Math.max(1, Math.round(image.height * scale));
  const rgb = new Float64Array(width * height * 3);

  for (let y = 0; y < height; y++) {
    const y0 = Math.floor((y * image.height) / height);
    const y1 = Math.max(y0 + 1, Math.floor(((y + 1) * image.height) / height));
    for (let x = 0; x < width; x++) {
      const x0 = Math.floor((x * image.width) / width);
      const x1 = Math.max(x0 + 1, Math.floor(((x + 1) * image.width) / width));
      let r = 0;
      let g = 0;
      let b = 0;
      for (let sy = y0; sy < y1; sy++) {
        for (let sx = x0; sx < x1; sx++) {
          const i = (sy * image.width + sx) * 4;
          const alpha = image.rgba[i + 3] / 255;
          r += image.rgba[i] * alpha;
          g += image.rgba[i + 1] * alpha;
          b += image.rgba[i + 2] * alpha;
        }
      }
      const count = (y1 - y0) * (x1 - x0);
      rgb.set([r / count, g / count, b / count], (y * width + x) * 3);
    }
  }

  return { width, height, rgb };
}

function toRgb565(r, g, b) {
  return (Math.round((r * 31) / 255) << 11) | (Math.round((g * 63) / 255) << 5) | Math.round((b * 31) / 255);
}

// Writes the C source for platform/circle/circle_splash.cpp. The file is only
// rewritten when its content changes, so unchanged images do not recompile.
export function writeSplashSource(pngFile, maxWidth, maxHeight, outFile) {
  const image = fitToScreen(decodePng(pngFile), maxWidth, maxHeight);
  const values = [];
  for (let i = 0; i < image.width * image.height; i++) {
    const value = toRgb565(image.rgb[i * 3], image.rgb[i * 3 + 1], image.rgb[i * 3 + 2]);
    values.push(`0x${value.toString(16).padStart(4, "0")}`);
  }

  const rows = [];
  for (let i = 0; i < values.length; i += 12) {
    rows.push(`\t${values.slice(i, i + 12).join(", ")},`);
  }

  const source = [
    `// Generated by baremetal/splash.mjs from ${path.basename(pngFile)}, do not edit.`,
    "#include <stdint.h>",
    "",
    `const unsigned g_SplashWidth = ${image.width};`,
    `const unsigned g_SplashHeight = ${image.height};`,
    `const uint16_t g_SplashPixels[${values.length}] = {`,
    ...rows,
    "};",
    "",
  ].join("\n");

  fs.mkdirSync(path.dirname(outFile), { recursive: true });
  if (!fs.existsSync(outFile) || fs.readFileSync(outFile, "utf8") !== source) {
    fs.writeFileSync(outFile, source);
  }

  return { width: image.width, height: image.height };
}
