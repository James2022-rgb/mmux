# mmux

MP4 / ISOBMFF muxer. Mirror of mdemux: wraps L-SMASH behind a
small C++ surface that hides L-SMASH from public headers
entirely (pimpl). One class per stream type. Today the only
class is `Mp4HevcVideoMuxer` (HEVC video track only); audio /
multi-track support can grow alongside their demuxer
counterparts.

Designed to pair with mhevcenc: feed the encoded VPS / SPS /
PPS bytes once at Open, then per-frame `AppendAccessUnit` with
the Annex B slice bytes returned from
`HevcEncodeSession::EncodePicture`. The muxer converts to
length-prefixed MP4 form internally and constructs the `hvcC`
sample description.

## Language

All code comments MUST be written in English.

## Symbols

It is forbidden to use the full width forms of symbols that have
counterparts in ASCII. e.g. `()`, `:`, `,`, `0-9`.

## Coding style

- `m<short>` library naming.
- `src/<libname>/{public,private}` layout; public include path is
  reached as `#include "mmux/public/<file>.h"`.
- TU header (`#include "<this file>.h"`) listed first in each
  `.cpp`, then C++ system headers, then external headers, then
  public project headers, then private project headers -- each
  group separated by a blank line.
- Slabs of related includes are grouped and labeled with the
  same `// public project headers --...` style markers used in
  the existing files.
- Public headers MUST NOT include `lsmash.h`. L-SMASH state is
  hidden behind a pimpl `struct Impl` defined in the `.cpp`.

## Build

CMake-based C++23 static library. Typically consumed as a
sibling-lib `add_subdirectory(... mmux ...)` from a parent
project's `CMakeLists.txt`. The parent **MUST** add `mbase`
first -- mmux's `CMakeLists.txt` asserts the target already
exists.

Target name: `mmux`. Links `mbase` PUBLIC, `lsmash` PRIVATE.

L-SMASH is pulled in via `FetchContent` if the `lsmash` target
does not already exist (i.e. mdemux or another sibling library
has not already added it). Both libs use the same source list
so whoever wins the `if(NOT TARGET lsmash)` race ends up
defining the single shared static library.

When adding source files to `CMakeLists.txt`, list them in
alphabetical order within each `set(...)` block.

## Directory structure

```
src/mmux/
  public/   <- API surface (no L-SMASH leaks)
    mp4_hevc_video_muxer.h
  private/  <- implementation (touches L-SMASH freely)
    mp4_hevc_video_muxer.cpp
```

## Public API

- `Mp4HevcVideoMuxConfig` -- input to `Mp4HevcVideoMuxer::Open`.
  Carries: `width` / `height` (coded luma samples), media
  `timescale` (ticks per second, e.g. 30000), `sample_duration`
  (ticks per frame, e.g. 1000 for 30 fps when timescale = 30000),
  the encoder-generated Annex B VPS+SPS+PPS blob (typically the
  bytes returned by `mhevcenc::HevcEncodeSession::vps_sps_pps_bytes()`),
  and the HEVC profile / tier / level identifiers that go into
  the `hvcC` sample description.
- `Mp4HevcVideoMuxer::Open(path, config) -> unique_ptr` --
  creates the output file, initializes the movie + track +
  sample table, builds the `hvcC` from the supplied parameter
  set NALs.
- `AppendAccessUnit(au_bytes, au_size, is_irap)` -- appends one
  HEVC access unit. Caller supplies Annex B-framed slice NAL(s);
  muxer converts to length-prefixed MP4 form internally and
  tags the sample with `ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC`
  when `is_irap` is true.
- `Close()` -- finalizes the moov box. Called automatically by
  the destructor; expose explicitly so callers can detect
  finalize errors (return value).

The public types live in namespace `mmux`.

## Dependencies

- `mbase` -- assertions, logging. Linked PUBLIC. Provided by
  the parent; mmux asserts the target exists.
- `L-SMASH` (`l-smash/l-smash` master) -- ISOBMFF library,
  pulled in via FetchContent if `lsmash` is not already a
  target. Linked PRIVATE; consumers never see L-SMASH headers
  because the public types use pimpl. The full source list
  matches mdemux's so the two libs share one `lsmash` target.

## License

L-SMASH is ISC-licensed (permissive, attribution required).
mmux itself adds no extra license constraint beyond mbase
and the project the consumer chooses.

## Threading

L-SMASH's `lsmash_root_t` is **not** internally thread-safe.
Each muxer instance owns its own root, so multiple instances
on different paths can be used from different threads. Calls
on the **same** instance must be serialized by the caller.
`AppendAccessUnit` is the mutating call to watch.

## Commit messages

Conventional-Commits-style with the lib as the scope, e.g.
`feat(mmux): ...`, `fix(mmux): ...`, `perf(mmux): ...`,
`docs(mmux): ...`, `refactor(mmux): ...`.
