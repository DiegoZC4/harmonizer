# Public Harmonizer

This static browser edition uses the visitor's microphone and Web MIDI locally.
It does not send audio to a server. Pitch shifting runs in an AudioWorklet via
the vendored Signalsmith Stretch Web build.

The native `harmonizer_web` application remains the reference performance
edition. It uses aubio and Rubber Band on macOS; this public edition uses a
browser YIN detector and Signalsmith Stretch so it can run from a static host.

Audio input, audio output, and hardware MIDI input are selected from menus.
Numeric controls use diagonal draggable-number behavior. Control values,
formant preservation, viewport settings, and device selections persist in
`localStorage` for the site origin.
The labeled computer-keyboard map starts at F3 by default and can be moved with
the **Keyboard octave** draggable number.

Test the pure pitch detector with:

```sh
node public/test-pitch.mjs
```
