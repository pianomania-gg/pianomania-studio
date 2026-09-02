# How this repository's tree was produced, and what it cost

Read this before refreshing the fork against a newer upstream MuseScore. The
first refresh lost 113 files without saying so, and the mechanism that lost them
is still in place.

## What happened

The first commit here, `d1b961cb`, is not an upstream checkout with Pianomania
changes applied. It is a copy of the `MuseScore/` directory as the pianomania
monorepo had it, and specifically of the files the monorepo was **tracking**:

```
monorepo:  git ls-files MuseScore/          -> 16046 files
this repo: git ls-tree -r --name-only d1b96 -> 16046 files
difference in either direction              -> 0
```

An export built from a git index cannot see an untracked file. Anything the
monorepo was not tracking, for any reason, was simply not there to copy — and
nothing reported it, because from the export's point of view nothing was
missing.

## What was lost, and why

113 files that upstream v4.7.4 has were absent from `d1b961cb`. Four unrelated
causes, none of them visible from inside this repository:

| Count | Files | Cause |
| --- | --- | --- |
| 99 | musicxml test PDFs, `share/manual`, KDDockWidgets docs | Monorepo commit `113dc75e7`, "removed musescore pdfs from git tracking", untracked them deliberately. They survive on disk only as 130-byte Git LFS pointer stubs, because the monorepo's root `.gitattributes` — a Unity GameCI template — routes `*.pdf` through LFS. |
| 6 | `.vscode_template/*.json`, `src/project/tests/data/from_meta*/audiosettings.json` and `viewsettings.json` | The monorepo's root `.gitignore` rules `*settings.json` and `*extensions.json`. They exist for Unity project settings and matched these by accident. |
| 1 | `…/mini_chromium/build/build_config.h` | `MuseScore/.gitignore`'s bare `build` rule. |
| 7 | mscz/mscx icon PNGs and SVGs, two partial-tie scores, one vtest score | Never tracked in the monorepo at all. |

One further file only looked missing.
`src/framework/learn/qml/Muse/Learn/resources/marc_sabatella.jpg` is present, as
`marc_sabatella.JPG`. A case-insensitive filesystem hid the difference; Linux
and macOS builders would not have. It is the only case-only difference in the
tree.

## Why this matters for the next refresh

None of the four causes has gone away. Refreshing this fork by exporting the
monorepo's tracked `MuseScore/` files again will drop the same files again, plus
whatever else has become untracked there since — and it will do so silently.

Two ways out, in order of preference:

1. **Build the fork tree from an upstream checkout**, applying Pianomania's
   changes on top, rather than copying the monorepo's vendored directory. Then
   the upstream half is upstream's by construction and only the fork's own
   changes need review.

2. **Keep the current method, but verify the result.** After producing the tree,
   compare it against the upstream tag and require every difference to be
   accounted for:

   ```bash
   git -C <upstream-checkout> ls-tree -r --name-only <tag> | sort > /tmp/up.txt
   git ls-tree -r --name-only HEAD                         | sort > /tmp/fork.txt
   comm -23 /tmp/up.txt /tmp/fork.txt        # in upstream, missing here
   ```

   Compare case-insensitively as well, or a rename like the `.JPG` above passes
   unnoticed:

   ```bash
   comm -23 <(tr 'A-Z' 'a-z' < /tmp/up.txt) <(tr 'A-Z' 'a-z' < /tmp/fork.txt)
   ```

A file being absent is not by itself a problem — most of the 113 are
documentation nothing reads. The problem is not knowing, because a missing test
fixture and a missing PDF look identical until someone checks what reads them.
