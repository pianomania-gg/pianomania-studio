# Suites measured against upstream reference data

Four unit-test suites are reported by CI but do not decide a pull request:

- `engraving_tests`
- `iex_mei_tests`
- `iex_midi_tests`
- `iex_musicxml_tests`

`buildscripts/ci/linux/runutests.sh` runs every other suite first and returns
that result, then runs these four and prints what they say.

## Why they fail

Each of these suites works the same way. It opens a score, saves it, and
compares the saved file character by character against a reference file stored
next to the test. The reference files came from upstream MuseScore and still
hold upstream's output.

This fork changes what a saved score contains. Three changes account for every
failure:

| Change | Appears as | Suites affected |
| --- | --- | --- |
| A hand assignment is written on every note and rest | `<pianomaniaHand>1</pianomaniaHand>` | engraving, midi, musicxml |
| A new score starts at a 1.778 spatium instead of 1.75 | `<spatium>1.778</spatium>` | mei |
| Exported MEI carries Pianomania geometry | `pm:x1y1x2y2`, `pm:bezier`, `pm:coveredUuids`, `pm:xy` | mei |

In the 2026-09-02 CI run these four suites produced 815 failed assertions
between them. Every one was a comparison that differed by the changes above.
None was a defect.

The suites are worth keeping. They are the only cover this repo has over
engraving output, and engraving is exactly what this fork modifies. They are
not gating because a permanently red gate cannot tell anyone that something new
broke.

## How to make them decide pull requests again

Re-record the reference files against this fork's output. A failing comparison
already writes the file it produced into the ctest working directory, so the
material is there after one run.

1. Build and run the suites, from `build.debug`:

   ```bash
   ctest -V -R '^(engraving_tests|iex_mei_tests|iex_midi_tests|iex_musicxml_tests)$'
   ```

2. For each failure, copy the produced file over its reference. The two names
   differ per suite, so check the `diff -u` line the failure prints — it names
   the reference first and the produced file second:

   | Suite | Reference | Produced |
   | --- | --- | --- |
   | engraving | `beam_data/Beam-A.mscx` | `Beam-A.mscx` |
   | mei | `data/accid-01.mei` | `accid-01.test.mei` |
   | midi | `midiimport_data/m1-ref.mscx` | `m1.mscx` |
   | musicxml | `data/testArpOnRest_ref.mscx` | `testArpOnRest.mscx` |

3. Re-run. The suites should be clean.

4. Remove `UPSTREAM_REFERENCE_TESTS` from `runutests.sh` so a single `ctest -V`
   decides the gate again.

## All three changes are deliberate

Re-recording a reference file makes the behaviour it captures permanent, so each
change was traced back to the code that produces it before this document
recommended re-recording anything.

The spatium is the one that looks most like drift, and it is not.
`src/engraving/style/styledef.cpp` takes it from
`pm::PmPageGeometry::spatiumLayoutUnits` where upstream hardcodes
`1.75 /*mm*/ * DPMM`. That constant is 0.07 inches, which is 1.778 mm, and it
sits in `src/engraving/pm/pmstyle.h` beside the rest of a page format designed
for a screen rather than for paper:

```cpp
static constexpr double pageWidthIn  = 10.0;   // 10 x 7.5 in, landscape
static constexpr double pageHeightIn = 7.5;
static constexpr double pageMarginIn = 0.591;
static constexpr double spatiumIn    = 0.07;
```

The same block hides headers, footers, and instrument names. Matt confirmed the
page format is intended.

The other two changes are what this fork exists to produce: the hand assignment
on every note and rest, and the geometry attributes on exported MEI.

## What still gates

Everything else, including the three suites that cover this fork's own work:

- `project_test` — the Pianomania coordinated export
- `iex_mei_pianomania_tests` — the MEI additions
- `iex_midi_pianomania_tests` — the MIDI export

A green unit-test check means those passed, along with the fifteen upstream
suites that do not compare against stored scores.
