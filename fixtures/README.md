# Harmonizer Audio Fixtures

This folder contains offline audio fixtures for pitch-detection testing.

## Vocadito

`fixtures/vocadito` is an optional, ignored local copy of Vocadito, a small
dataset of 40 solo, monophonic singing excerpts with frame-level F0
annotations, note annotations, lyrics, and language metadata.

Source: https://zenodo.org/records/5578807
License: Creative Commons Attribution 4.0 International
The useful paths are:

- `vocadito/Audio/*.wav`
- `vocadito/Annotations/F0/*.csv`
- `vocadito/Annotations/Notes/*.csv`
- `vocadito/Annotations/Lyrics/*.txt`
- `vocadito/vocadito_metadata.csv`

Per-run analyzer output goes in `pitch_reports/`.

## Refresh

```bash
./scripts/fetch_vocadito.sh
```

The helper verifies Zenodo's published checksum before extracting the dataset.
