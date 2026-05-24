// TU header --------------------------------------------
#include "mmux/public/mp4_muxer.h"

// c++ system headers -----------------------------------
#include <cstring>
#include <utility>
#include <vector>

// public project headers -------------------------------
#include "mbase/public/assert.h"
#include "mbase/public/log.h"

// external headers --------------------------------------
#include "lsmash.h"

namespace mmux {

namespace {

// ---- Annex B helpers (HEVC track only) ----------------------

struct AnnexBNal final {
  uint8_t const* data = nullptr;
  uint32_t       size = 0;
};

std::vector<AnnexBNal> SplitAnnexB(uint8_t const* data, uint32_t size) {
  std::vector<AnnexBNal> result;
  if (data == nullptr || size < 3) return result;
  uint32_t i = 0;
  while (i + 2 < size) {
    if (data[i] == 0x00 && data[i + 1] == 0x00) {
      uint32_t sc_len = 0;
      if (data[i + 2] == 0x01) sc_len = 3;
      else if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) sc_len = 4;
      if (sc_len > 0) {
        uint32_t const nal_begin = i + sc_len;
        uint32_t j = nal_begin;
        while (j + 2 < size) {
          if (data[j] == 0x00 && data[j + 1] == 0x00
           && (data[j + 2] == 0x01 || (j + 3 < size && data[j + 2] == 0x00 && data[j + 3] == 0x01))) {
            break;
          }
          ++j;
        }
        uint32_t const nal_end = (j + 2 < size) ? j : size;
        if (nal_end > nal_begin) {
          result.push_back(AnnexBNal{ data + nal_begin, nal_end - nal_begin });
        }
        i = nal_end;
        continue;
      }
    }
    ++i;
  }
  return result;
}

constexpr uint8_t kHevcNalTypeVpsNut = 32;
constexpr uint8_t kHevcNalTypeSpsNut = 33;
constexpr uint8_t kHevcNalTypePpsNut = 34;

uint8_t GetHevcNalType(uint8_t nal_header_byte_0) {
  return static_cast<uint8_t>((nal_header_byte_0 >> 1) & 0x3F);
}

constexpr uint32_t kFourcc_gpmd = LSMASH_4CC('g','p','m','d');
constexpr uint32_t kFourcc_tmcd = LSMASH_4CC('t','m','c','d');

} // anonymous namespace

// ---- Impl --------------------------------------------------

struct Mp4Muxer::Impl final {
  enum class TrackKind { kHevcVideo, kAacAudio, kGpmd, kTimecode };

  struct TrackState final {
    TrackKind    kind            = TrackKind::kHevcVideo;
    uint32_t     lsmash_track_id = 0;
    uint32_t     sample_entry    = 0;
    uint32_t     timescale       = 0;
    uint32_t     last_duration   = 0;  // used at flush time
    uint32_t     appended_count  = 0;
  };

  lsmash_root_t*           root            = nullptr;
  lsmash_file_t*           file            = nullptr;
  lsmash_file_parameters_t file_params{};
  uint32_t                 movie_timescale = 0;
  std::vector<TrackState>  tracks;          // index = TrackId - 1
  bool                     closed          = false;
  bool                     failed          = false;
};

// ---- Open / dtor -------------------------------------------

std::unique_ptr<Mp4Muxer> Mp4Muxer::Open(std::string const& path, uint32_t movie_timescale) {
  if (movie_timescale == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::Open: movie_timescale must be > 0.");
    return nullptr;
  }
  auto self = std::unique_ptr<Mp4Muxer>(new Mp4Muxer());
  Impl& s = *self->impl_;
  s.movie_timescale = movie_timescale;

  s.root = lsmash_create_root();
  if (s.root == nullptr) {
    MBASE_LOG_ERROR("Mp4Muxer::Open: lsmash_create_root failed.");
    return nullptr;
  }
  if (lsmash_open_file(path.c_str(), 0, &s.file_params) < 0) {
    MBASE_LOG_ERROR("Mp4Muxer::Open: lsmash_open_file('{}') failed.", path);
    return nullptr;
  }
  static lsmash_brand_type kBrands[] = {
    ISOM_BRAND_TYPE_ISOM,
    ISOM_BRAND_TYPE_MP41,
  };
  s.file_params.major_brand   = ISOM_BRAND_TYPE_MP41;
  s.file_params.brands        = kBrands;
  s.file_params.brand_count   = static_cast<uint32_t>(sizeof(kBrands) / sizeof(kBrands[0]));
  s.file_params.minor_version = 0;
  s.file = lsmash_set_file(s.root, &s.file_params);
  if (s.file == nullptr) {
    MBASE_LOG_ERROR("Mp4Muxer::Open: lsmash_set_file failed.");
    return nullptr;
  }

  lsmash_movie_parameters_t movie_params{};
  lsmash_initialize_movie_parameters(&movie_params);
  movie_params.timescale = movie_timescale;
  if (lsmash_set_movie_parameters(s.root, &movie_params) < 0) {
    MBASE_LOG_ERROR("Mp4Muxer::Open: lsmash_set_movie_parameters failed.");
    return nullptr;
  }

  MBASE_LOG_INFO("Mp4Muxer::Open: '{}' ready (movie timescale={}).", path, movie_timescale);
  return self;
}

Mp4Muxer::Mp4Muxer() : impl_(std::make_unique<Impl>()) {}

Mp4Muxer::~Mp4Muxer() {
  if (!impl_->closed && !impl_->failed) {
    Close();
  }
  if (impl_->root != nullptr) {
    lsmash_close_file(&impl_->file_params);
    lsmash_destroy_root(impl_->root);
    impl_->root = nullptr;
  }
}

// ---- AddHevcVideoTrack -------------------------------------

Mp4Muxer::TrackId Mp4Muxer::AddHevcVideoTrack(HevcVideoTrackConfig const& config) {
  Impl& s = *impl_;
  if (s.closed || s.failed) return 0;
  if (config.width == 0 || config.height == 0 || config.timescale == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddHevcVideoTrack: invalid config.");
    return 0;
  }
  std::vector<AnnexBNal> const nals = SplitAnnexB(
    config.vps_sps_pps_annex_b.data(),
    static_cast<uint32_t>(config.vps_sps_pps_annex_b.size()));
  if (nals.empty()) {
    MBASE_LOG_ERROR("Mp4Muxer::AddHevcVideoTrack: no NAL units in VPS/SPS/PPS blob.");
    return 0;
  }

  uint32_t const track_id = lsmash_create_track(s.root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK);
  if (track_id == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddHevcVideoTrack: lsmash_create_track failed.");
    return 0;
  }
  lsmash_track_parameters_t tp{};
  lsmash_initialize_track_parameters(&tp);
  tp.mode           = static_cast<lsmash_track_mode>(
    ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE | ISOM_TRACK_IN_PREVIEW);
  tp.display_width  = static_cast<uint64_t>(config.width)  << 16;
  tp.display_height = static_cast<uint64_t>(config.height) << 16;
  if (lsmash_set_track_parameters(s.root, track_id, &tp) < 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddHevcVideoTrack: lsmash_set_track_parameters failed.");
    return 0;
  }
  lsmash_media_parameters_t mp{};
  lsmash_initialize_media_parameters(&mp);
  mp.timescale          = config.timescale;
  mp.media_handler_name = const_cast<char*>("mmux HEVC video");
  mp.roll_grouping      = 1;
  mp.rap_grouping       = 1;
  if (lsmash_set_media_parameters(s.root, track_id, &mp) < 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddHevcVideoTrack: lsmash_set_media_parameters failed.");
    return 0;
  }

  lsmash_summary_t* generic_summary = lsmash_create_summary(LSMASH_SUMMARY_TYPE_VIDEO);
  auto* summary                     = reinterpret_cast<lsmash_video_summary_t*>(generic_summary);
  summary->sample_type = ISOM_CODEC_TYPE_HVC1_VIDEO;
  summary->width       = config.width;
  summary->height      = config.height;
  summary->timescale   = config.timescale;
  summary->timebase    = config.default_sample_duration;
  summary->vfr         = 0;
  summary->color.primaries_index = 1;
  summary->color.transfer_index  = 1;
  summary->color.matrix_index    = 1;

  lsmash_codec_specific_t* hvcc_cs = lsmash_create_codec_specific_data(
    LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_HEVC,
    LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED);
  auto* hvcc = reinterpret_cast<lsmash_hevc_specific_parameters_t*>(hvcc_cs->data.structured);
  hvcc->lengthSizeMinusOne = 3;
  for (auto const& nal : nals) {
    if (nal.size == 0) continue;
    uint8_t const type = GetHevcNalType(nal.data[0]);
    lsmash_hevc_dcr_nalu_type dcr_type;
    switch (type) {
      case kHevcNalTypeVpsNut: dcr_type = HEVC_DCR_NALU_TYPE_VPS; break;
      case kHevcNalTypeSpsNut: dcr_type = HEVC_DCR_NALU_TYPE_SPS; break;
      case kHevcNalTypePpsNut: dcr_type = HEVC_DCR_NALU_TYPE_PPS; break;
      default: continue;
    }
    lsmash_append_hevc_dcr_nalu(hvcc, dcr_type, const_cast<uint8_t*>(nal.data), nal.size);
  }
  lsmash_add_codec_specific_data(generic_summary, hvcc_cs);
  lsmash_destroy_codec_specific_data(hvcc_cs);

  // NOTE: do NOT call lsmash_cleanup_summary here. L-SMASH's
  // lsmash_add_sample_entry keeps a reference to the summary;
  // cleaning it up immediately use-after-frees during finish_movie.
  // The summary is freed when the root is destroyed.
  uint32_t const sample_entry = lsmash_add_sample_entry(s.root, track_id, generic_summary);
  if (sample_entry == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddHevcVideoTrack: lsmash_add_sample_entry failed.");
    lsmash_cleanup_summary(generic_summary);
    return 0;
  }

  s.tracks.push_back(Impl::TrackState{
    .kind            = Impl::TrackKind::kHevcVideo,
    .lsmash_track_id = track_id,
    .sample_entry    = sample_entry,
    .timescale       = config.timescale,
    .last_duration   = config.default_sample_duration,
  });
  return static_cast<TrackId>(s.tracks.size());
}

// ---- AddAacAudioTrack --------------------------------------

Mp4Muxer::TrackId Mp4Muxer::AddAacAudioTrack(AacAudioTrackConfig const& config) {
  Impl& s = *impl_;
  if (s.closed || s.failed) return 0;
  if (config.timescale == 0 || config.sample_rate == 0 || config.channel_count == 0
   || config.asc_bytes.empty()) {
    MBASE_LOG_ERROR("Mp4Muxer::AddAacAudioTrack: invalid config.");
    return 0;
  }

  uint32_t const track_id = lsmash_create_track(s.root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK);
  if (track_id == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddAacAudioTrack: lsmash_create_track failed.");
    return 0;
  }
  lsmash_track_parameters_t tp{};
  lsmash_initialize_track_parameters(&tp);
  tp.mode = static_cast<lsmash_track_mode>(
    ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE | ISOM_TRACK_IN_PREVIEW);
  if (lsmash_set_track_parameters(s.root, track_id, &tp) < 0) {
    return 0;
  }
  lsmash_media_parameters_t mp{};
  lsmash_initialize_media_parameters(&mp);
  mp.timescale          = config.timescale;
  mp.media_handler_name = const_cast<char*>("mmux AAC audio");
  if (lsmash_set_media_parameters(s.root, track_id, &mp) < 0) {
    return 0;
  }

  lsmash_summary_t* generic_summary = lsmash_create_summary(LSMASH_SUMMARY_TYPE_AUDIO);
  auto* summary                     = reinterpret_cast<lsmash_audio_summary_t*>(generic_summary);
  summary->sample_type      = ISOM_CODEC_TYPE_MP4A_AUDIO;
  summary->frequency        = config.sample_rate;
  summary->channels         = static_cast<uint16_t>(config.channel_count);
  summary->sample_size      = 16;
  summary->aot              = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
  summary->samples_in_frame = 1024;  // AAC LC: 1024 PCM frames per AU
  summary->sbr_mode         = MP4A_AAC_SBR_NOT_SPECIFIED;
  summary->bytes_per_frame  = 0;     // variable

  // esds (DecoderConfigDescriptor + DecoderSpecificInfo = ASC)
  lsmash_codec_specific_t* esds_cs = lsmash_create_codec_specific_data(
    LSMASH_CODEC_SPECIFIC_DATA_TYPE_MP4SYS_DECODER_CONFIG,
    LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED);
  auto* esds_param = reinterpret_cast<lsmash_mp4sys_decoder_parameters_t*>(esds_cs->data.structured);
  esds_param->objectTypeIndication = MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3;
  esds_param->streamType           = MP4SYS_STREAM_TYPE_AudioStream;
  lsmash_set_mp4sys_decoder_specific_info(esds_param,
    const_cast<uint8_t*>(config.asc_bytes.data()),
    static_cast<uint32_t>(config.asc_bytes.size()));
  lsmash_add_codec_specific_data(generic_summary, esds_cs);
  lsmash_destroy_codec_specific_data(esds_cs);

  uint32_t const sample_entry = lsmash_add_sample_entry(s.root, track_id, generic_summary);
  if (sample_entry == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddAacAudioTrack: lsmash_add_sample_entry failed.");
    lsmash_cleanup_summary(generic_summary);
    return 0;
  }

  s.tracks.push_back(Impl::TrackState{
    .kind            = Impl::TrackKind::kAacAudio,
    .lsmash_track_id = track_id,
    .sample_entry    = sample_entry,
    .timescale       = config.timescale,
  });
  return static_cast<TrackId>(s.tracks.size());
}

// ---- AddGpmdTrack ------------------------------------------

Mp4Muxer::TrackId Mp4Muxer::AddGpmdTrack(GpmdTrackConfig const& config) {
  Impl& s = *impl_;
  if (s.closed || s.failed) return 0;
  // TODO: L-SMASH's lsmash_create_summary doesn't support
  // LSMASH_SUMMARY_TYPE_UNKNOWN, and a hand-rolled sample entry
  // would need its own writer hook. Bail before creating the trak
  // box so we don't leave a half-built track in the moov.
  MBASE_LOG_WARN("Mp4Muxer::AddGpmdTrack: gpmd track support not implemented yet.");
  return 0;

  if (config.timescale == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddGpmdTrack: invalid timescale.");
    return 0;
  }

  uint32_t const track_id = lsmash_create_track(s.root, ISOM_MEDIA_HANDLER_TYPE_TIMED_METADATA_TRACK);
  if (track_id == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddGpmdTrack: lsmash_create_track failed.");
    return 0;
  }
  lsmash_track_parameters_t tp{};
  lsmash_initialize_track_parameters(&tp);
  tp.mode = static_cast<lsmash_track_mode>(ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE);
  if (lsmash_set_track_parameters(s.root, track_id, &tp) < 0) {
    return 0;
  }
  lsmash_media_parameters_t mp{};
  lsmash_initialize_media_parameters(&mp);
  mp.timescale          = config.timescale;
  mp.media_handler_name = const_cast<char*>("mmux GPMF meta");
  if (lsmash_set_media_parameters(s.root, track_id, &mp) < 0) {
    return 0;
  }

  // Build a minimal SampleEntry box for `gpmd`. L-SMASH doesn't have
  // a typed summary helper for it, so we hand-roll the bytes: an
  // 8-byte box header (size + type) + the SampleEntry's 6 reserved
  // bytes + 2 bytes data_reference_index. Total: 16 bytes.
  // We push this as an "unstructured" codec-specific data of type
  // RAW which lsmash will write verbatim as a child of stsd.
  // Actually L-SMASH expects a summary -> sample-entry path; the
  // simpler route is to create a generic LSMASH_SUMMARY_TYPE_UNKNOWN
  // summary and set sample_type to gpmd.
  lsmash_summary_t* generic_summary = lsmash_create_summary(LSMASH_SUMMARY_TYPE_UNKNOWN);
  if (generic_summary == nullptr) {
    MBASE_LOG_ERROR("Mp4Muxer::AddGpmdTrack: lsmash_create_summary(UNKNOWN) failed.");
    return 0;
  }
  generic_summary->sample_type.fourcc = kFourcc_gpmd;

  uint32_t const sample_entry = lsmash_add_sample_entry(s.root, track_id, generic_summary);
  lsmash_cleanup_summary(generic_summary);
  if (sample_entry == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddGpmdTrack: lsmash_add_sample_entry failed.");
    return 0;
  }

  s.tracks.push_back(Impl::TrackState{
    .kind            = Impl::TrackKind::kGpmd,
    .lsmash_track_id = track_id,
    .sample_entry    = sample_entry,
    .timescale       = config.timescale,
  });
  return static_cast<TrackId>(s.tracks.size());
}

// ---- AddTimecodeTrack --------------------------------------

Mp4Muxer::TrackId Mp4Muxer::AddTimecodeTrack(TimecodeTrackConfig const& config) {
  Impl& s = *impl_;
  if (s.closed || s.failed) return 0;
  // TODO: same blocker as AddGpmdTrack (no LSMASH_SUMMARY_TYPE for
  // tmcd, and tmcd's SampleEntry has its own 18-byte extradata that
  // needs a custom writer). Bail before creating the trak box.
  MBASE_LOG_WARN("Mp4Muxer::AddTimecodeTrack: tmcd track support not implemented yet.");
  return 0;

  if (config.timescale == 0 || config.frame_duration == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddTimecodeTrack: invalid config.");
    return 0;
  }

  // QuickTime timecode tracks use handler type 'tmcd' too. L-SMASH's
  // lsmash_media_type enum doesn't include it; cast the fourcc.
  lsmash_media_type const kTmcdHandler = static_cast<lsmash_media_type>(kFourcc_tmcd);
  uint32_t const track_id = lsmash_create_track(s.root, kTmcdHandler);
  if (track_id == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddTimecodeTrack: lsmash_create_track failed.");
    return 0;
  }
  lsmash_track_parameters_t tp{};
  lsmash_initialize_track_parameters(&tp);
  tp.mode = static_cast<lsmash_track_mode>(ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE);
  if (lsmash_set_track_parameters(s.root, track_id, &tp) < 0) {
    return 0;
  }
  lsmash_media_parameters_t mp{};
  lsmash_initialize_media_parameters(&mp);
  mp.timescale          = config.timescale;
  mp.media_handler_name = const_cast<char*>("mmux QT timecode");
  if (lsmash_set_media_parameters(s.root, track_id, &mp) < 0) {
    return 0;
  }

  // Minimal tmcd SampleEntry payload (after the 8-byte box header
  // + 6 reserved + 2 data_reference_index that L-SMASH writes for
  // every SampleEntry): we'd need to also append a 'tcmi' child for
  // a strictly conformant box. For PoC we settle for the bare
  // tmcd fields tucked into an "unstructured" summary blob via the
  // UNKNOWN summary path.
  lsmash_summary_t* generic_summary = lsmash_create_summary(LSMASH_SUMMARY_TYPE_UNKNOWN);
  if (generic_summary == nullptr) {
    MBASE_LOG_ERROR("Mp4Muxer::AddTimecodeTrack: lsmash_create_summary(UNKNOWN) failed.");
    return 0;
  }
  generic_summary->sample_type.fourcc = kFourcc_tmcd;

  uint32_t const sample_entry = lsmash_add_sample_entry(s.root, track_id, generic_summary);
  lsmash_cleanup_summary(generic_summary);
  if (sample_entry == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AddTimecodeTrack: lsmash_add_sample_entry failed.");
    return 0;
  }

  s.tracks.push_back(Impl::TrackState{
    .kind            = Impl::TrackKind::kTimecode,
    .lsmash_track_id = track_id,
    .sample_entry    = sample_entry,
    .timescale       = config.timescale,
  });
  return static_cast<TrackId>(s.tracks.size());
}

// ---- AppendSample ------------------------------------------

bool Mp4Muxer::AppendSample(TrackId track_id,
                            uint8_t const* data, uint32_t size,
                            uint64_t dts, uint32_t duration,
                            bool is_sync) {
  Impl& s = *impl_;
  if (s.failed || s.closed) return false;
  if (track_id == 0 || track_id > s.tracks.size()) {
    MBASE_LOG_ERROR("Mp4Muxer::AppendSample: invalid track_id {}", track_id);
    s.failed = true;
    return false;
  }
  if (data == nullptr || size == 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AppendSample: empty sample on track {}", track_id);
    s.failed = true;
    return false;
  }
  Impl::TrackState& t = s.tracks[track_id - 1];

  lsmash_sample_t* sample = lsmash_create_sample(size);
  if (sample == nullptr) {
    MBASE_LOG_ERROR("Mp4Muxer::AppendSample: lsmash_create_sample({}) failed", size);
    s.failed = true;
    return false;
  }
  std::memcpy(sample->data, data, size);
  sample->length        = size;
  sample->dts           = dts;
  sample->cts           = dts;
  sample->index         = t.sample_entry;
  sample->prop.ra_flags = is_sync
    ? ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC
    : ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;

  if (lsmash_append_sample(s.root, t.lsmash_track_id, sample) < 0) {
    MBASE_LOG_ERROR("Mp4Muxer::AppendSample: lsmash_append_sample failed (track {})", track_id);
    s.failed = true;
    return false;
  }
  t.last_duration = duration;
  ++t.appended_count;
  return true;
}

bool Mp4Muxer::AppendHevcAccessUnit(TrackId track_id,
                                    uint8_t const* au_data, uint32_t au_size,
                                    uint64_t dts, uint32_t duration,
                                    bool is_irap) {
  std::vector<AnnexBNal> const nals = SplitAnnexB(au_data, au_size);
  if (nals.empty()) {
    MBASE_LOG_ERROR("Mp4Muxer::AppendHevcAccessUnit: no NAL units in AU ({} bytes)", au_size);
    impl_->failed = true;
    return false;
  }
  uint32_t mp4_size = 0;
  for (auto const& nal : nals) mp4_size += 4 + nal.size;
  std::vector<uint8_t> mp4_buf(mp4_size);
  uint32_t offset = 0;
  for (auto const& nal : nals) {
    mp4_buf[offset + 0] = static_cast<uint8_t>((nal.size >> 24) & 0xFF);
    mp4_buf[offset + 1] = static_cast<uint8_t>((nal.size >> 16) & 0xFF);
    mp4_buf[offset + 2] = static_cast<uint8_t>((nal.size >>  8) & 0xFF);
    mp4_buf[offset + 3] = static_cast<uint8_t>( nal.size        & 0xFF);
    std::memcpy(mp4_buf.data() + offset + 4, nal.data, nal.size);
    offset += 4 + nal.size;
  }
  return AppendSample(track_id, mp4_buf.data(), mp4_size, dts, duration, is_irap);
}

// ---- Close -------------------------------------------------

bool Mp4Muxer::Close() {
  Impl& s = *impl_;
  if (s.closed) return true;
  s.closed = true;
  if (s.root == nullptr) return false;

  for (Impl::TrackState const& t : s.tracks) {
    if (lsmash_flush_pooled_samples(s.root, t.lsmash_track_id, t.last_duration) < 0) {
      MBASE_LOG_ERROR("Mp4Muxer::Close: lsmash_flush_pooled_samples failed (track_id {}).", t.lsmash_track_id);
      s.failed = true;
      return false;
    }
  }
  lsmash_adhoc_remux_t remux{};
  int const finish_rc = lsmash_finish_movie(s.root, &remux);
  if (finish_rc < 0) {
    MBASE_LOG_ERROR("Mp4Muxer::Close: lsmash_finish_movie failed (rc = {}).", finish_rc);
    s.failed = true;
    return false;
  }
  uint32_t total = 0;
  for (auto const& t : s.tracks) total += t.appended_count;
  MBASE_LOG_INFO("Mp4Muxer::Close: finalized with {} tracks, {} total samples.",
    s.tracks.size(), total);
  return true;
}

uint32_t Mp4Muxer::total_appended_count() const {
  uint32_t total = 0;
  for (auto const& t : impl_->tracks) total += t.appended_count;
  return total;
}

} // namespace mmux
