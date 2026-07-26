export const clamp = (value, min, max) => Math.min(max, Math.max(min, value));

export function frequencyToMidi(frequency) {
  return frequency > 0 ? 69 + 12 * Math.log2(frequency / 440) : -1;
}

export function midiToFrequency(note) {
  return 440 * (2 ** ((note - 69) / 12));
}

export function midiName(note) {
  const names = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"];
  const rounded = Math.round(note);
  return `${names[((rounded % 12) + 12) % 12]}${Math.floor(rounded / 12) - 1}`;
}

export function median(values) {
  const valid = values.filter(Number.isFinite).sort((a, b) => a - b);
  if (!valid.length) return NaN;
  return valid[Math.floor(valid.length / 2)];
}

export function rmsOf(samples) {
  let sum = 0;
  for (let index = 0; index < samples.length; index += 1) {
    sum += samples[index] * samples[index];
  }
  return Math.sqrt(sum / Math.max(1, samples.length));
}

// YIN's cumulative mean normalized difference function is robust enough for
// live monophonic voice tracking without adding another runtime dependency.
export function detectPitch(samples, sampleRate, options = {}) {
  const minHz = options.minHz ?? midiToFrequency(33);
  const maxHz = options.maxHz ?? midiToFrequency(84);
  const threshold = options.threshold ?? 0.13;
  const rms = rmsOf(samples);
  if (rms < (options.gate ?? 0)) return { frequency: -1, clarity: 0, rms };

  const maxTau = Math.min(Math.floor(samples.length / 2), Math.floor(sampleRate / minHz));
  const minTau = Math.max(2, Math.floor(sampleRate / maxHz));
  if (maxTau <= minTau + 2) return { frequency: -1, clarity: 0, rms };

  const difference = new Float64Array(maxTau + 1);
  for (let tau = 1; tau <= maxTau; tau += 1) {
    let sum = 0;
    const limit = samples.length - tau;
    for (let index = 0; index < limit; index += 1) {
      const delta = samples[index] - samples[index + tau];
      sum += delta * delta;
    }
    difference[tau] = sum;
  }

  let runningSum = 0;
  difference[0] = 1;
  for (let tau = 1; tau <= maxTau; tau += 1) {
    runningSum += difference[tau];
    difference[tau] = runningSum > 0 ? difference[tau] * tau / runningSum : 1;
  }

  let tau = -1;
  for (let candidate = minTau; candidate < maxTau; candidate += 1) {
    if (difference[candidate] < threshold) {
      tau = candidate;
      while (tau + 1 <= maxTau && difference[tau + 1] < difference[tau]) tau += 1;
      break;
    }
  }
  if (tau < 0) {
    let best = minTau;
    for (let candidate = minTau + 1; candidate <= maxTau; candidate += 1) {
      if (difference[candidate] < difference[best]) best = candidate;
    }
    if (difference[best] > 0.28) return { frequency: -1, clarity: 0, rms };
    tau = best;
  }

  const left = difference[Math.max(1, tau - 1)];
  const center = difference[tau];
  const right = difference[Math.min(maxTau, tau + 1)];
  const denominator = left - 2 * center + right;
  const refinedTau = Math.abs(denominator) > 1e-12
    ? tau + 0.5 * (left - right) / denominator
    : tau;
  const frequency = sampleRate / refinedTau;
  if (!Number.isFinite(frequency) || frequency < minHz || frequency > maxHz) {
    return { frequency: -1, clarity: 0, rms };
  }
  return { frequency, clarity: clamp(1 - center, 0, 1), rms };
}
