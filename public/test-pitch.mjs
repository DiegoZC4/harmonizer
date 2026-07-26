import assert from "node:assert/strict";
import { detectPitch, frequencyToMidi, median, midiToFrequency } from "./pitch.js";

function sine(frequency, sampleRate = 48000, length = 4096) {
  return Float32Array.from({ length }, (_, index) => 0.4 * Math.sin(2 * Math.PI * frequency * index / sampleRate));
}

for (const frequency of [55, 110, 220, 440, 523.251]) {
  const result = detectPitch(sine(frequency), 48000, { gate: 0.001 });
  assert.ok(result.frequency > 0, `expected ${frequency} Hz to be detected`);
  assert.ok(Math.abs(frequencyToMidi(result.frequency) - frequencyToMidi(frequency)) < 0.08,
    `expected ${frequency} Hz within 0.08 semitone, got ${result.frequency}`);
  assert.ok(result.clarity > 0.9, `expected a clear sine at ${frequency} Hz`);
}

const silence = detectPitch(new Float32Array(4096), 48000, { gate: 0.001 });
assert.equal(silence.frequency, -1);
assert.equal(median([NaN, 3, 1, 2]), 2);
assert.ok(Math.abs(midiToFrequency(69) - 440) < 1e-9);
console.log("public harmonizer pitch tests passed");
