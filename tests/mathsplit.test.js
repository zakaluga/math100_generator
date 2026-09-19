// task-13: node-тест splitWideMathChains (JS-зеркало mathsplit.cpp).
// Фикстуры: tests/testdata/mathsplit_cases.txt (общие с C++-тестом).
// Запуск из корня проекта: node tests/mathsplit.test.js
"use strict";

const fs = require("fs");
const path = require("path");

// Загружаем каноническую функцию (IIFE, ставит window.splitWideMathChains).
const src = fs.readFileSync(
    path.join(__dirname, "..", "frontend", "js", "mathsplit.js"),
    "utf8"
);
global.window = {};
eval(src);
if (typeof global.window.splitWideMathChains !== "function") {
    console.error("FAIL: window.splitWideMathChains не определена");
    process.exit(2);
}
const split = global.window.splitWideMathChains;

const fixture = path.join(__dirname, "testdata", "mathsplit_cases.txt");
const lines = fs.readFileSync(fixture, "utf8").split("\n").filter((l) => l.trim() !== "" && !l.startsWith("#"));

let total = 0;
let failed = 0;
lines.forEach((line, i) => {
    const no = i + 1;
    const parts = line.split("\t");
    if (parts.length !== 3 || parts[0] !== "IN") {
        console.warn(`line ${no}: malformed, skipped`);
        return;
    }
    const input = parts[1];
    const expected = parts[2];
    const actual = split(input);
    total++;
    if (actual === expected) {
        console.log(`PASS [${no}]`);
    } else {
        failed++;
        console.error(`FAIL [${no}]`);
        console.error("  input:    " + input);
        console.error("  expected: " + expected);
        console.error("  actual:   " + actual);
    }
});

if (failed === 0) {
    console.log(`ALL PASS (${total}/10)`);
    process.exit(0);
}
console.error(`FAILURES: ${failed}/${total}`);
process.exit(1);
