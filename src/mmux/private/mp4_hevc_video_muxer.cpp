// TU header --------------------------------------------
#include "mmux/public/mp4_hevc_video_muxer.h"

// c++ system headers -----------------------------------
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// public project headers -------------------------------
#include "mbase/public/assert.h"
#include "mbase/public/log.h"

// external headers --------------------------------------
#include "lsmash.h"

namespace mmux {

namespace {

/// Walk a buffer in Annex B form (3- or 4-byte start codes between NAL
/// units) and return a vector of [begin, end) byte ranges, one per NAL
/// unit, with the start code stripped.
struct AnnexBNal final {
  uint8_t const* data = nullptr;  // first byte after the start code (= NAL header byte)
  uint32_t       size = 0;
};

std::vector<AnnexBNal> SplitAnnexB(uint8_t const* data, uint32_t size) {
  std::vector<AnnexBNal> result;
  if (data == nullptr || size < 3) return result;

  // Scan for start codes (0x000001 or 0x00000001).
  uint32_t i = 0;
  while (i + 2 < size) {
    if (data[i] == 0x00 && data[i + 1] == 0x00) {
      uint32_t sc_len = 0;
      if (data[i + 2] == 0x01) {
        sc_len = 3;
      } else if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
        sc_len = 4;
      }
      if (sc_len > 0) {
        uint32_t const nal_begin = i + sc_len;
        // Find next start code (or end of buffer) to bound the current NAL.
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
  // HEVC NAL header: forbidden_zero_bit (1) | nal_unit_type (6) | nuh_layer_id_msb (1)
  // For nuh_layer_id_msb = 0 (always today), nal_unit_type = (byte0 >> 1) & 0x3F.
  return static_cast<uint8_t>((nal_header_byte_0 >> 1) & 0x3F);
}

} // anonymous namespace

struct Mp4HevcVideoMuxer::Impl final {
  lsmash_root_t*           root            = nullptr;
  lsmash_file_t*           file            = nullptr;
  lsmash_file_parameters_t file_params{};
  uint32_t                 track_id        = 0;
  uint32_t                 sample_entry    = 0;
  lsmash_video_summary_t*  summary         = nullptr;
  uint32_t                 timescale       = 0;
  uint32_t                 sample_duration = 0;
  uint32_t                 width           = 0;
  uint32_t                 height          = 0;
  uint64_t                 next_dts        = 0;
  uint32_t                 appended_count  = 0;
  bool                     closed          = false;
  bool                     failed          = false;
};

std::unique_ptr<Mp4HevcVideoMuxer> Mp4HevcVideoMuxer::Open(
  std::string const& path,
  Mp4HevcVideoMuxConfig const& config
) {
  if (config.width == 0 || config.height == 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: invalid dimensions ({}x{}).", config.width, config.height);
    return nullptr;
  }
  if (config.timescale == 0 || config.sample_duration == 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: invalid timescale {} / sample_duration {}.",
      config.timescale, config.sample_duration);
    return nullptr;
  }
  std::vector<AnnexBNal> const nals = SplitAnnexB(
    config.vps_sps_pps_annex_b.data(),
    static_cast<uint32_t>(config.vps_sps_pps_annex_b.size())
  );
  if (nals.empty()) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: no NAL units found in vps_sps_pps_annex_b blob ({} bytes).",
      config.vps_sps_pps_annex_b.size());
    return nullptr;
  }

  auto self = std::unique_ptr<Mp4HevcVideoMuxer>(new Mp4HevcVideoMuxer());
  Impl& s = *self->impl_;
  s.timescale       = config.timescale;
  s.sample_duration = config.sample_duration;
  s.width           = config.width;
  s.height          = config.height;

  // --- L-SMASH root + file ---
  s.root = lsmash_create_root();
  if (s.root == nullptr) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_create_root failed.");
    return nullptr;
  }
  if (lsmash_open_file(path.c_str(), 0, &s.file_params) < 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_open_file('{}') failed.", path);
    return nullptr;
  }
  // Brand list: ISOM + MP41 covers HEVC-in-MP4 for typical players.
  // L-SMASH does not surface an HVC1 brand constant; the sample-entry
  // codec type alone (ISOM_CODEC_TYPE_HVC1_VIDEO) tells decoders this
  // is HEVC.
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
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_set_file failed.");
    return nullptr;
  }

  // --- Movie parameters ---
  lsmash_movie_parameters_t movie_params{};
  lsmash_initialize_movie_parameters(&movie_params);
  movie_params.timescale = config.timescale;
  if (lsmash_set_movie_parameters(s.root, &movie_params) < 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_set_movie_parameters failed.");
    return nullptr;
  }

  // --- Track ---
  s.track_id = lsmash_create_track(s.root, ISOM_MEDIA_HANDLER_TYPE_VIDEO_TRACK);
  if (s.track_id == 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_create_track failed.");
    return nullptr;
  }
  lsmash_track_parameters_t track_params{};
  lsmash_initialize_track_parameters(&track_params);
  track_params.mode           = static_cast<lsmash_track_mode>(
      ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE | ISOM_TRACK_IN_PREVIEW);
  track_params.display_width  = static_cast<uint64_t>(config.width)  << 16;
  track_params.display_height = static_cast<uint64_t>(config.height) << 16;
  if (lsmash_set_track_parameters(s.root, s.track_id, &track_params) < 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_set_track_parameters failed.");
    return nullptr;
  }
  lsmash_media_parameters_t media_params{};
  lsmash_initialize_media_parameters(&media_params);
  media_params.timescale          = config.timescale;
  media_params.media_handler_name = const_cast<char*>("mmux HEVC video");
  media_params.roll_grouping      = 1;
  media_params.rap_grouping       = 1;
  if (lsmash_set_media_parameters(s.root, s.track_id, &media_params) < 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_set_media_parameters failed.");
    return nullptr;
  }

  // --- Video summary + hvcC ---
  lsmash_summary_t* generic_summary = lsmash_create_summary(LSMASH_SUMMARY_TYPE_VIDEO);
  if (generic_summary == nullptr) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_create_summary failed.");
    return nullptr;
  }
  s.summary = reinterpret_cast<lsmash_video_summary_t*>(generic_summary);
  s.summary->sample_type = ISOM_CODEC_TYPE_HVC1_VIDEO;
  s.summary->width       = config.width;
  s.summary->height      = config.height;
  s.summary->timescale   = config.timescale;
  s.summary->timebase    = config.sample_duration;
  s.summary->vfr         = 0;
  s.summary->color.primaries_index = 1;  // BT.709 default; HDR callers should plumb this
  s.summary->color.transfer_index  = 1;
  s.summary->color.matrix_index    = 1;

  // hvcC sample description: append the supplied VPS/SPS/PPS NALs.
  // lsmash_append_hevc_dcr_nalu auto-fills profile/tier/level/etc. from
  // the SPS bytes; we only need to set lengthSizeMinusOne explicitly.
  lsmash_codec_specific_t* hvcc_cs = lsmash_create_codec_specific_data(
    LSMASH_CODEC_SPECIFIC_DATA_TYPE_ISOM_VIDEO_HEVC,
    LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED
  );
  if (hvcc_cs == nullptr) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_create_codec_specific_data(HEVC) failed.");
    lsmash_cleanup_summary(generic_summary);
    return nullptr;
  }
  auto* hvcc_param = reinterpret_cast<lsmash_hevc_specific_parameters_t*>(hvcc_cs->data.structured);
  hvcc_param->lengthSizeMinusOne = 3;  // 4-byte length prefix per NAL in samples
  for (auto const& nal : nals) {
    if (nal.size == 0) continue;
    uint8_t const type = GetHevcNalType(nal.data[0]);
    lsmash_hevc_dcr_nalu_type dcr_type;
    switch (type) {
    case kHevcNalTypeVpsNut: dcr_type = HEVC_DCR_NALU_TYPE_VPS; break;
    case kHevcNalTypeSpsNut: dcr_type = HEVC_DCR_NALU_TYPE_SPS; break;
    case kHevcNalTypePpsNut: dcr_type = HEVC_DCR_NALU_TYPE_PPS; break;
    default:
      MBASE_LOG_WARN("Mp4HevcVideoMuxer::Open: skipping unexpected NAL type {} in vps_sps_pps blob.", type);
      continue;
    }
    if (lsmash_append_hevc_dcr_nalu(
          hvcc_param, dcr_type,
          const_cast<uint8_t*>(nal.data), nal.size) < 0) {
      MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_append_hevc_dcr_nalu(type={}) failed.", static_cast<int>(dcr_type));
      lsmash_destroy_codec_specific_data(hvcc_cs);
      lsmash_cleanup_summary(generic_summary);
      return nullptr;
    }
  }
  if (lsmash_add_codec_specific_data(generic_summary, hvcc_cs) < 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_add_codec_specific_data failed.");
    lsmash_destroy_codec_specific_data(hvcc_cs);
    lsmash_cleanup_summary(generic_summary);
    return nullptr;
  }
  lsmash_destroy_codec_specific_data(hvcc_cs);

  s.sample_entry = lsmash_add_sample_entry(s.root, s.track_id, generic_summary);
  if (s.sample_entry == 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Open: lsmash_add_sample_entry failed.");
    lsmash_cleanup_summary(generic_summary);
    return nullptr;
  }

  MBASE_LOG_INFO("Mp4HevcVideoMuxer::Open: '{}' ready ({}x{}, timescale={}, sample_duration={}, nals={}).",
    path, config.width, config.height, config.timescale, config.sample_duration, nals.size());
  return self;
}

Mp4HevcVideoMuxer::Mp4HevcVideoMuxer() : impl_(std::make_unique<Impl>()) {}

Mp4HevcVideoMuxer::~Mp4HevcVideoMuxer() {
  if (!impl_->closed && !impl_->failed) {
    Close();
  }
  if (impl_->root != nullptr) {
    lsmash_close_file(&impl_->file_params);
    lsmash_destroy_root(impl_->root);
    impl_->root = nullptr;
  }
}

bool Mp4HevcVideoMuxer::AppendAccessUnit(
  uint8_t const* au_data, uint32_t au_size, bool is_irap
) {
  Impl& s = *impl_;
  if (s.failed) return false;
  if (s.closed) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::AppendAccessUnit called after Close.");
    return false;
  }
  if (au_data == nullptr || au_size == 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::AppendAccessUnit: empty AU.");
    return false;
  }

  // Convert Annex B (NAL units with start-code prefixes) to MP4 form
  // (NAL units with 4-byte big-endian length prefixes).
  std::vector<AnnexBNal> const nals = SplitAnnexB(au_data, au_size);
  if (nals.empty()) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::AppendAccessUnit: no NAL units found in AU ({} bytes).", au_size);
    s.failed = true;
    return false;
  }
  uint32_t mp4_size = 0;
  for (auto const& nal : nals) mp4_size += 4 + nal.size;

  lsmash_sample_t* sample = lsmash_create_sample(mp4_size);
  if (sample == nullptr) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::AppendAccessUnit: lsmash_create_sample({}) failed.", mp4_size);
    s.failed = true;
    return false;
  }
  uint32_t offset = 0;
  for (auto const& nal : nals) {
    sample->data[offset + 0] = static_cast<uint8_t>((nal.size >> 24) & 0xFF);
    sample->data[offset + 1] = static_cast<uint8_t>((nal.size >> 16) & 0xFF);
    sample->data[offset + 2] = static_cast<uint8_t>((nal.size >>  8) & 0xFF);
    sample->data[offset + 3] = static_cast<uint8_t>( nal.size        & 0xFF);
    std::memcpy(sample->data + offset + 4, nal.data, nal.size);
    offset += 4 + nal.size;
  }
  sample->length  = mp4_size;
  sample->dts     = s.next_dts;
  sample->cts     = s.next_dts;  // no reordering for IDR + N x P GOP
  sample->index   = s.sample_entry;
  sample->prop.ra_flags = is_irap
    ? ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC
    : ISOM_SAMPLE_RANDOM_ACCESS_FLAG_NONE;

  if (lsmash_append_sample(s.root, s.track_id, sample) < 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::AppendAccessUnit: lsmash_append_sample failed.");
    s.failed = true;
    return false;
  }
  s.next_dts += s.sample_duration;
  ++s.appended_count;
  return true;
}

bool Mp4HevcVideoMuxer::Close() {
  Impl& s = *impl_;
  if (s.closed) return true;
  s.closed = true;
  if (s.root == nullptr) return false;

  if (lsmash_flush_pooled_samples(s.root, s.track_id, s.sample_duration) < 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Close: lsmash_flush_pooled_samples failed.");
    s.failed = true;
    return false;
  }
  lsmash_adhoc_remux_t remux{};
  if (lsmash_finish_movie(s.root, &remux) < 0) {
    MBASE_LOG_ERROR("Mp4HevcVideoMuxer::Close: lsmash_finish_movie failed.");
    s.failed = true;
    return false;
  }
  MBASE_LOG_INFO("Mp4HevcVideoMuxer::Close: finalized with {} appended samples.", s.appended_count);
  return true;
}

uint32_t Mp4HevcVideoMuxer::appended_count() const {
  return impl_->appended_count;
}

} // namespace mmux
