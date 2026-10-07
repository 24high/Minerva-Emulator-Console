#!/usr/bin/env node
// Builds and runs tests/button_mapper_test.cpp with the host compiler.
//
// usage: node baremetal/tests/button-mapper-test.mjs
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const testsDir = path.dirname(fileURLToPath(import.meta.url));
const projectRoot = path.resolve(testsDir, "..");
const outDir = path.join(projectRoot, "build-node", "host-tests");
fs.mkdirSync(outDir, { recursive: true });

const binary = path.join(outDir, "button_mapper_test");
const compile = spawnSync(process.env.CXX || "c++", [
  "-std=c++11", "-Wall", "-Wextra", "-Werror",
  "-I", projectRoot,
  "-I", path.join(projectRoot, "..", "src", "libretro-common", "include"),
  "-o", binary, path.join(testsDir, "button_mapper_test.cpp"),
], { stdio: "inherit" });
if (compile.status !== 0) process.exit(1);

const result = spawnSync(binary, [], { stdio: "inherit" });
process.exit(result.status ?? 1);
