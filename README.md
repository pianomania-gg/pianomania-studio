# Pianomania Studio

Pianomania Studio is the score editor that ships with [Pianomania](https://pianomania.gg). It is a fork of [MuseScore Studio](https://github.com/musescore/MuseScore) trimmed to piano notation, with export tooling that produces the coordinated score, MIDI, and metadata files the game consumes.

This repository is the complete corresponding source for every distributed Pianomania Studio build. Each release of the application has a matching tag here.

## Upstream and license

- Upstream base: MuseScore Studio v4.7.4 (see `UPSTREAM_BASE`)
- License: GNU General Public License version 3 (see `LICENSE.txt`)

MuseScore and MuseScore Studio are trademarks of their respective owners. Pianomania Studio is an independent fork and is not affiliated with or endorsed by Muse Group.

## Building

Pianomania Studio builds the same way as upstream MuseScore Studio; see `buildscripts/`. Release builds for Apple Silicon macOS are produced with `buildscripts/local/prepare-macos-arm64-app.sh`.
