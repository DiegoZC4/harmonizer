# Third-Party Notices

Harmonizer uses the following third-party projects. Release binaries include
the project license and corresponding Harmonizer source.

| Project | Use | License |
| --- | --- | --- |
| [Rubber Band Library](https://breakfastquay.com/rubberband/) | Native pitch shifting | GPL-2.0-or-later |
| [aubio](https://aubio.org/) | Native pitch detection | GPL-3.0-or-later |
| [PortAudio](https://www.portaudio.com/) | Native audio I/O | PortAudio license |
| [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | Browser pitch shifting and experimental native backend | MIT |

Rubber Band 4.0.0 is fetched from its official source archive and verified by
SHA-256 during native builds. The browser distribution includes Signalsmith's
license at `public/vendor/LICENSE-signalsmith-stretch.txt`.

The optional Vocadito test dataset is not distributed in this repository or
release binaries. Its download helper retrieves it from Zenodo under
CC BY 4.0; attribution details are in `fixtures/README.md`.
