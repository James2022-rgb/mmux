#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mmux {

/// Input to `Mp4HevcVideoMuxer::Open`. Carries the codec / track
/// parameters the muxer needs to construct the `hvcC` sample
/// description and to compute per-sample timestamps.
struct Mp4HevcVideoMuxConfig final {
  /// Coded luma sample dimensions. Written into the track header's
  /// display extents and into the visual sample entry's width / height.
  uint32_t width  = 0;
  uint32_t height = 0;

  /// Media timescale (ticks per second). 30000 accommodates 29.97 /
  /// 30 / 59.94 / 60 fps cleanly. 90000 is the legacy MPEG choice.
  uint32_t timescale = 30000;

  /// Sample duration in `timescale` ticks. The nominal frame rate is
  /// `timescale / sample_duration` (e.g. timescale = 30000,
  /// sample_duration = 1000 -> 30 fps). All samples receive this same
  /// duration (suitable for constant-frame-rate output; variable-fr
  /// muxing would need a per-AppendAccessUnit duration parameter).
  uint32_t sample_duration = 1000;

  /// Annex B parameter-set blob containing the VPS + SPS + PPS NAL
  /// units (each prefixed with a 3- or 4-byte start code). Typically
  /// the bytes returned by `mhevcenc::HevcEncodeSession::vps_sps_pps_bytes()`.
  /// The muxer parses out each NAL and feeds it into the `hvcC`
  /// arrays; the blob itself is not stored in the file.
  std::vector<uint8_t> vps_sps_pps_annex_b;
};

/// MP4 / ISOBMFF muxer for a single HEVC video track. Use case:
/// real-time encode -> open this once, AppendAccessUnit per frame,
/// Close at end (or let the destructor finalize). The muxer hides
/// L-SMASH entirely.
class Mp4HevcVideoMuxer final {
public:
  /// Opens `path` for writing and initializes the movie + track +
  /// sample description (including the `hvcC` built from
  /// `config.vps_sps_pps_annex_b`). Returns `nullptr` on file-open
  /// failure, invalid config, or hvcC construction error.
  static std::unique_ptr<Mp4HevcVideoMuxer> Open(
    std::string const& path,
    Mp4HevcVideoMuxConfig const& config);

  /// Destructor implicitly finalizes the moov and closes the file.
  /// Errors during finalize are logged but cannot be returned;
  /// callers that need to detect them should call `Close()` explicitly.
  ~Mp4HevcVideoMuxer();
  Mp4HevcVideoMuxer(Mp4HevcVideoMuxer const&) = delete;
  Mp4HevcVideoMuxer& operator=(Mp4HevcVideoMuxer const&) = delete;

  /// Appends one HEVC access unit. `au_data` contains the slice NAL(s)
  /// in Annex B form (3-byte `00 00 01` or 4-byte `00 00 00 01` start
  /// code per NAL); the muxer rewrites the start codes into 4-byte
  /// length prefixes inline. `is_irap` true if this is an IDR or other
  /// random-access entry; tagged with `ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC`.
  ///
  /// Returns true on success. After a failure further calls are
  /// rejected and `Close()` will not produce a valid file.
  bool AppendAccessUnit(uint8_t const* au_data, uint32_t au_size, bool is_irap);

  /// Flushes pooled samples, finalizes the moov box, and closes the
  /// underlying file. Idempotent (subsequent calls are no-ops). Returns
  /// true on success.
  bool Close();

  /// 1-indexed number of access units the muxer has accepted so far.
  /// Useful for caller diagnostics / progress display.
  uint32_t appended_count() const;

private:
  Mp4HevcVideoMuxer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mmux
