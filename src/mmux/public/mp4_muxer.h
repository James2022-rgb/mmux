#pragma once

// c++ system headers -----------------------------------
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mmux {

/// Multi-track MP4 / ISOBMFF muxer.
///
/// Sibling of `Mp4HevcVideoMuxer` for callers that need to assemble
/// an output file with more than one stream. Tracks are added once
/// (returns a 1-indexed `TrackId`), then samples are appended via the
/// type-erased `AppendSample` with per-sample DTS / duration / sync
/// flag chosen by the caller. The muxer pools samples internally per
/// L-SMASH's normal interleaving and emits them all on `Close`.
///
/// Currently supports the four track types the GoPro re-encode PoC
/// needs:
///   - HEVC video (re-encoded)
///   - AAC audio (passthrough)
///   - `gpmd` timed-metadata (GoPro GPMF passthrough)
///   - `tmcd` QuickTime timecode (passthrough)
///
/// The class hides L-SMASH from its public header entirely (pimpl).
class Mp4Muxer final {
public:
  using TrackId = uint32_t;  // 1-indexed; 0 = invalid

  /// Opens `path` for writing and initializes the root + file +
  /// movie header (no tracks yet). Caller drives `Add*Track` once
  /// per stream, then per-sample `AppendSample`, then `Close`.
  /// Movie timescale is supplied at Open so per-track timescales
  /// can be reasoned about consistently. 1000 (== 1 ms ticks) is
  /// a reasonable default; 30000 matches common video tracks.
  static std::unique_ptr<Mp4Muxer> Open(std::string const& path, uint32_t movie_timescale);

  ~Mp4Muxer();
  Mp4Muxer(Mp4Muxer const&) = delete;
  Mp4Muxer& operator=(Mp4Muxer const&) = delete;

  // ---- Track types ----------------------------------------------

  struct HevcVideoTrackConfig final {
    uint32_t width                = 0;
    uint32_t height               = 0;
    uint32_t timescale            = 30000;
    uint32_t default_sample_duration = 1000;  // also written into the track's edit-list
    /// VPS+SPS+PPS Annex B blob from the encoder (e.g. from
    /// `mhevcenc::HevcEncodeSession::vps_sps_pps_bytes()`).
    std::vector<uint8_t> vps_sps_pps_annex_b;
  };
  TrackId AddHevcVideoTrack(HevcVideoTrackConfig const& config);

  struct AacAudioTrackConfig final {
    uint32_t timescale     = 0;  // typically equals sample_rate
    uint32_t sample_rate   = 0;
    uint32_t channel_count = 0;
    /// AudioSpecificConfig bytes from the source `esds` box.
    /// Pass `mdemux::Mp4AacAudioDemuxer::GetAscBytes()` directly.
    std::vector<uint8_t> asc_bytes;
  };
  TrackId AddAacAudioTrack(AacAudioTrackConfig const& config);

  /// GoPro GPMF metadata track (handler = `meta`, codec = `gpmd`).
  /// L-SMASH has no built-in helpers for this codec; the muxer
  /// builds a minimal SampleEntry with the `gpmd` fourcc itself.
  struct GpmdTrackConfig final {
    uint32_t timescale = 0;
  };
  TrackId AddGpmdTrack(GpmdTrackConfig const& config);

  /// QuickTime timecode track (handler = `tmcd`, codec = `tmcd`).
  /// The sample is a single big-endian 32-bit timecode value;
  /// the SampleEntry carries the frame-rate metadata required
  /// for a decoder to render hh:mm:ss:ff.
  struct TimecodeTrackConfig final {
    uint32_t timescale          = 0;
    /// Per the QuickTime tmcd format: how many timescale ticks make
    /// one frame. `timescale / frame_duration` == nominal fps.
    uint32_t frame_duration     = 0;
    /// Number of timecode frames per integer second (e.g. 30 for
    /// 30000/1001 fps).
    uint8_t  fps_int            = 30;
    /// QuickTime tmcd flags. Bit 0 = drop frame, bit 1 = max-frames-24,
    /// bit 2 = negative-times-OK, bit 3 = counter. 0 is fine for most
    /// integer-fps captures.
    uint32_t flags              = 0;
  };
  TrackId AddTimecodeTrack(TimecodeTrackConfig const& config);

  /// Add a track whose entire sample-description box is copied
  /// verbatim from a source MP4. Useful for codecs L-SMASH already
  /// recognises (e.g. MJPEG, ProRes via QT codec types) when we
  /// want bit-exact passthrough without reconstructing the
  /// `lsmash_summary_t` from individual fields.
  ///
  /// Internally opens `source_path` with its own L-SMASH root,
  /// iterates the source's tracks looking for the first one whose
  /// sample-entry fourcc matches `codec_fourcc`, and re-uses the
  /// returned summary to seed the output track. The source root
  /// stays alive inside the muxer for the rest of the session
  /// because L-SMASH's summary objects borrow from the root.
  ///
  /// `media_handler_name` is written into the output mdia's hdlr
  /// box. Pass an empty string to use a generic default.
  ///
  /// Returns 0 if the source can't be opened or no matching track
  /// is found.
  ///
  /// **Known limitation** -- L-SMASH's public read API drops
  /// genuinely unknown sample entries (e.g. `gpmd` GoPro
  /// telemetry, `tmcd` QuickTime timecode) into the parent box's
  /// `extensions` rather than `stsd->list`, and
  /// `lsmash_get_summary` returns NULL for entries it can't
  /// classify as video, audio, or hint. Multiple-file patches
  /// across read / summary / write paths would be needed to
  /// unlock those codecs; until that happens this method returns
  /// 0 for gpmd / tmcd inputs even though they're present in the
  /// source file.
  TrackId AddPassthroughTrackByCodec(std::string const& source_path,
                                     uint32_t codec_fourcc,
                                     std::string const& media_handler_name = {});

  // ---- Sample append --------------------------------------------

  /// Appends one sample to `track_id`. `dts` is in the track's own
  /// timescale ticks; `duration` is the sample's nominal length in
  /// the same ticks. `is_sync` should be true for video IRAPs and
  /// for every audio / metadata sample (those are inherently
  /// independently parseable). Returns false (and latches the muxer
  /// into a failed state) on any L-SMASH error.
  bool AppendSample(TrackId track_id,
                    uint8_t const* data, uint32_t size,
                    uint64_t dts, uint32_t duration,
                    bool is_sync = true);

  /// Convenience for HEVC video tracks: takes an Annex B access unit
  /// and rewrites the start codes into 4-byte length prefixes
  /// internally. `dts` is in the track's timescale (the muxer doesn't
  /// auto-advance one for you, so the caller stays in control of
  /// non-constant frame rates).
  bool AppendHevcAccessUnit(TrackId track_id,
                            uint8_t const* au_data, uint32_t au_size,
                            uint64_t dts, uint32_t duration,
                            bool is_irap);

  /// Flushes pooled samples, finalizes the moov, closes the file.
  /// Idempotent.
  bool Close();

  /// 1-indexed number of samples appended across all tracks.
  uint32_t total_appended_count() const;

private:
  Mp4Muxer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mmux
