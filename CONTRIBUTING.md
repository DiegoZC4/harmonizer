# Contributing

## Development loop

1. Install the platform dependencies from `docs/DEVELOPMENT.md`.
2. Configure and build with `cmake --preset test` and
   `cmake --build --preset test`.
3. Run `ctest --preset test` and `node public/test-pitch.mjs`.
4. Keep generated audio, captures, downloaded research media, and build output
   out of commits.
5. Describe audible behavior changes and the audio route used for manual tests
   in the pull request.

Small commits are encouraged. Avoid committing benchmark renders unless they
are intentionally promoted to a compact, documented regression fixture.

## Versioning

`VERSION.txt` is the single source of truth for release versions. Normal changes go
under `Unreleased` in `CHANGELOG.md`. See `docs/RELEASING.md` for the tag-driven
release process.
