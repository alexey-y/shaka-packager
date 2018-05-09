// Copyright 2017 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/mpd/base/representation.h"

#include <gflags/gflags.h>

#include <algorithm>

#include "packager/base/logging.h"
#include "packager/file/file.h"
#include "packager/media/base/muxer_util.h"
#include "packager/mpd/base/mpd_options.h"
#include "packager/mpd/base/mpd_utils.h"
#include "packager/mpd/base/xml/xml_node.h"

namespace shaka {
namespace {

std::string GetMimeType(const std::string& prefix,
                        MediaInfo::ContainerType container_type) {
  switch (container_type) {
    case MediaInfo::CONTAINER_MP4:
      return prefix + "/mp4";
    case MediaInfo::CONTAINER_MPEG2_TS:
      // NOTE: DASH MPD spec uses lowercase but RFC3555 says uppercase.
      return prefix + "/MP2T";
    case MediaInfo::CONTAINER_WEBM:
      return prefix + "/webm";
    default:
      break;
  }

  // Unsupported container types should be rejected/handled by the caller.
  LOG(ERROR) << "Unrecognized container type: " << container_type;
  return std::string();
}

// Check whether the video info has width and height.
// DASH IOP also requires several other fields for video representations, namely
// width, height, framerate, and sar.
bool HasRequiredVideoFields(const MediaInfo_VideoInfo& video_info) {
  if (!video_info.has_height() || !video_info.has_width()) {
    LOG(ERROR)
        << "Width and height are required fields for generating a valid MPD.";
    return false;
  }
  // These fields are not required for a valid MPD, but required for DASH IOP
  // compliant MPD. MpdBuilder can keep generating MPDs without these fields.
  LOG_IF(WARNING, !video_info.has_time_scale())
      << "Video info does not contain timescale required for "
         "calculating framerate. @frameRate is required for DASH IOP.";
  LOG_IF(WARNING, !video_info.has_pixel_width())
      << "Video info does not contain pixel_width to calculate the sample "
         "aspect ratio required for DASH IOP.";
  LOG_IF(WARNING, !video_info.has_pixel_height())
      << "Video info does not contain pixel_height to calculate the sample "
         "aspect ratio required for DASH IOP.";
  return true;
}

uint32_t GetTimeScale(const MediaInfo& media_info) {
  if (media_info.has_reference_time_scale()) {
    return media_info.reference_time_scale();
  }

  if (media_info.has_video_info()) {
    return media_info.video_info().time_scale();
  }

  if (media_info.has_audio_info()) {
    return media_info.audio_info().time_scale();
  }

  LOG(WARNING) << "No timescale specified, using 1 as timescale.";
  return 1;
}

uint64_t LastSegmentStartTime(const SegmentInfo& segment_info) {
  return segment_info.start_time + segment_info.duration * segment_info.repeat;
}

// This is equal to |segment_info| end time
uint64_t LastSegmentEndTime(const SegmentInfo& segment_info) {
  return segment_info.start_time +
         segment_info.duration * (segment_info.repeat + 1);
}

uint64_t LatestSegmentStartTime(const std::list<SegmentInfo>& segments) {
  DCHECK(!segments.empty());
  const SegmentInfo& latest_segment = segments.back();
  return LastSegmentStartTime(latest_segment);
}

// Given |timeshift_limit|, finds out the number of segments that are no longer
// valid and should be removed from |segment_info|.
uint64_t SearchTimedOutRepeatIndex(uint64_t timeshift_limit,
                                   const SegmentInfo& segment_info) {
  DCHECK_LE(timeshift_limit, LastSegmentEndTime(segment_info));
  if (timeshift_limit < segment_info.start_time)
    return 0;

  return (timeshift_limit - segment_info.start_time) / segment_info.duration;
}

}  // namespace

Representation::Representation(
    const MediaInfo& media_info,
    const MpdOptions& mpd_options,
    uint32_t id,
    std::unique_ptr<RepresentationStateChangeListener> state_change_listener)
    : media_info_(media_info),
      id_(id),
      bandwidth_estimator_(BandwidthEstimator::kUseAllBlocks),
      mpd_options_(mpd_options),
      state_change_listener_(std::move(state_change_listener)),
      allow_approximate_segment_timeline_(
          // TODO(kqyang): Need a better check. $Time is legitimate but not a
          // template.
          media_info.segment_template().find("$Time") == std::string::npos &&
          mpd_options_.mpd_params.allow_approximate_segment_timeline) {}

Representation::Representation(
    const Representation& representation,
    std::unique_ptr<RepresentationStateChangeListener> state_change_listener)
    : Representation(representation.media_info_,
                     representation.mpd_options_,
                     representation.id_,
                     std::move(state_change_listener)) {
  mime_type_ = representation.mime_type_;
  codecs_ = representation.codecs_;

  start_number_ = representation.start_number_;
  for (const SegmentInfo& segment_info : representation.segment_infos_)
    start_number_ += segment_info.repeat + 1;
}

Representation::~Representation() {}

bool Representation::Init() {
  if (!AtLeastOneTrue(media_info_.has_video_info(),
                      media_info_.has_audio_info(),
                      media_info_.has_text_info())) {
    // This is an error. Segment information can be in AdaptationSet, Period, or
    // MPD but the interface does not provide a way to set them.
    // See 5.3.9.1 ISO 23009-1:2012 for segment info.
    LOG(ERROR) << "Representation needs one of video, audio, or text.";
    return false;
  }

  if (MoreThanOneTrue(media_info_.has_video_info(),
                      media_info_.has_audio_info(),
                      media_info_.has_text_info())) {
    LOG(ERROR) << "Only one of VideoInfo, AudioInfo, or TextInfo can be set.";
    return false;
  }

  if (media_info_.container_type() == MediaInfo::CONTAINER_UNKNOWN) {
    LOG(ERROR) << "'container_type' in MediaInfo cannot be CONTAINER_UNKNOWN.";
    return false;
  }

  if (media_info_.has_video_info()) {
    mime_type_ = GetVideoMimeType();
    if (!HasRequiredVideoFields(media_info_.video_info())) {
      LOG(ERROR) << "Missing required fields to create a video Representation.";
      return false;
    }
  } else if (media_info_.has_audio_info()) {
    mime_type_ = GetAudioMimeType();
  } else if (media_info_.has_text_info()) {
    mime_type_ = GetTextMimeType();
  }

  if (mime_type_.empty())
    return false;

  codecs_ = GetCodecs(media_info_);
  return true;
}

void Representation::AddContentProtectionElement(
    const ContentProtectionElement& content_protection_element) {
  content_protection_elements_.push_back(content_protection_element);
  RemoveDuplicateAttributes(&content_protection_elements_.back());
}

void Representation::UpdateContentProtectionPssh(const std::string& drm_uuid,
                                                 const std::string& pssh) {
  UpdateContentProtectionPsshHelper(drm_uuid, pssh,
                                    &content_protection_elements_);
}

void Representation::AddNewSegment(uint64_t start_time,
                                   uint64_t duration,
                                   uint64_t size) {
  if (start_time == 0 && duration == 0) {
    LOG(WARNING) << "Got segment with start_time and duration == 0. Ignoring.";
    return;
  }

  if (state_change_listener_)
    state_change_listener_->OnNewSegmentForRepresentation(start_time, duration);

  AddSegmentInfo(start_time, duration);

  bandwidth_estimator_.AddBlock(
      size, static_cast<double>(duration) / media_info_.reference_time_scale());

  SlideWindow();
  DCHECK_GE(segment_infos_.size(), 1u);
}

void Representation::SetSampleDuration(uint32_t frame_duration) {
  // Sample duration is used to generate approximate SegmentTimeline.
  // Text is required to have exactly the same segment duration.
  if (media_info_.has_audio_info() || media_info_.has_video_info())
    frame_duration_ = frame_duration;

  if (media_info_.has_video_info()) {
    media_info_.mutable_video_info()->set_frame_duration(frame_duration);
    if (state_change_listener_) {
      state_change_listener_->OnSetFrameRateForRepresentation(
          frame_duration, media_info_.video_info().time_scale());
    }
  }
}

const MediaInfo& Representation::GetMediaInfo() const {
  return media_info_;
}

// Uses info in |media_info_| and |content_protection_elements_| to create a
// "Representation" node.
// MPD schema has strict ordering. The following must be done in order.
// AddVideoInfo() (possibly adds FramePacking elements), AddAudioInfo() (Adds
// AudioChannelConfig elements), AddContentProtectionElements*(), and
// AddVODOnlyInfo() (Adds segment info).
xml::scoped_xml_ptr<xmlNode> Representation::GetXml() {
  if (!HasRequiredMediaInfoFields()) {
    LOG(ERROR) << "MediaInfo missing required fields.";
    return xml::scoped_xml_ptr<xmlNode>();
  }

  const uint64_t bandwidth = media_info_.has_bandwidth()
                                 ? media_info_.bandwidth()
                                 : bandwidth_estimator_.Max();

  DCHECK(!(HasVODOnlyFields(media_info_) && HasLiveOnlyFields(media_info_)));

  xml::RepresentationXmlNode representation;
  // Mandatory fields for Representation.
  representation.SetId(id_);
  representation.SetIntegerAttribute("bandwidth", bandwidth);
  if (!codecs_.empty())
    representation.SetStringAttribute("codecs", codecs_);
  representation.SetStringAttribute("mimeType", mime_type_);

  const bool has_video_info = media_info_.has_video_info();
  const bool has_audio_info = media_info_.has_audio_info();

  if (has_video_info &&
      !representation.AddVideoInfo(
          media_info_.video_info(),
          !(output_suppression_flags_ & kSuppressWidth),
          !(output_suppression_flags_ & kSuppressHeight),
          !(output_suppression_flags_ & kSuppressFrameRate))) {
    LOG(ERROR) << "Failed to add video info to Representation XML.";
    return xml::scoped_xml_ptr<xmlNode>();
  }

  if (has_audio_info &&
      !representation.AddAudioInfo(media_info_.audio_info())) {
    LOG(ERROR) << "Failed to add audio info to Representation XML.";
    return xml::scoped_xml_ptr<xmlNode>();
  }

  if (!representation.AddContentProtectionElements(
          content_protection_elements_)) {
    return xml::scoped_xml_ptr<xmlNode>();
  }

  if (HasVODOnlyFields(media_info_) &&
      !representation.AddVODOnlyInfo(media_info_)) {
    LOG(ERROR) << "Failed to add VOD segment info.";
    return xml::scoped_xml_ptr<xmlNode>();
  }

  if (HasLiveOnlyFields(media_info_) &&
      !representation.AddLiveOnlyInfo(media_info_, segment_infos_,
                                      start_number_)) {
    LOG(ERROR) << "Failed to add Live info.";
    return xml::scoped_xml_ptr<xmlNode>();
  }
  // TODO(rkuroiwa): It is likely that all representations have the exact same
  // SegmentTemplate. Optimize and propagate the tag up to AdaptationSet level.

  output_suppression_flags_ = 0;
  return representation.PassScopedPtr();
}

void Representation::SuppressOnce(SuppressFlag flag) {
  output_suppression_flags_ |= flag;
}

void Representation::SetPresentationTimeOffset(
    double presentation_time_offset) {
  uint64_t pto = presentation_time_offset * media_info_.reference_time_scale();
  if (pto <= 0)
    return;
  media_info_.set_presentation_time_offset(pto);
}

bool Representation::GetStartAndEndTimestamps(
    double* start_timestamp_seconds,
    double* end_timestamp_seconds) const {
  if (segment_infos_.empty())
    return false;

  if (start_timestamp_seconds) {
    *start_timestamp_seconds =
        static_cast<double>(segment_infos_.begin()->start_time) /
        GetTimeScale(media_info_);
  }
  if (end_timestamp_seconds) {
    *end_timestamp_seconds =
        static_cast<double>(segment_infos_.rbegin()->start_time +
                            segment_infos_.rbegin()->duration *
                                (segment_infos_.rbegin()->repeat + 1)) /
        GetTimeScale(media_info_);
  }
  return true;
}

bool Representation::HasRequiredMediaInfoFields() const {
  if (HasVODOnlyFields(media_info_) && HasLiveOnlyFields(media_info_)) {
    LOG(ERROR) << "MediaInfo cannot have both VOD and Live fields.";
    return false;
  }

  if (!media_info_.has_container_type()) {
    LOG(ERROR) << "MediaInfo missing required field: container_type.";
    return false;
  }

  return true;
}

void Representation::AddSegmentInfo(uint64_t start_time, uint64_t duration) {
  const uint64_t kNoRepeat = 0;
  const uint64_t adjusted_duration = AdjustDuration(duration);

  if (!segment_infos_.empty()) {
    // Contiguous segment.
    const SegmentInfo& previous = segment_infos_.back();
    const uint64_t previous_segment_end_time =
        previous.start_time + previous.duration * (previous.repeat + 1);
    // Make it continuous if the segment start time is close to previous segment
    // end time.
    if (ApproximiatelyEqual(previous_segment_end_time, start_time)) {
      const uint64_t segment_end_time_for_same_duration =
          previous_segment_end_time + previous.duration;
      const uint64_t actual_segment_end_time = start_time + duration;
      // Consider the segments having identical duration if the segment end time
      // is close to calculated segment end time by assuming identical duration.
      if (ApproximiatelyEqual(segment_end_time_for_same_duration,
                              actual_segment_end_time)) {
        ++segment_infos_.back().repeat;
      } else {
        segment_infos_.push_back(
            {previous_segment_end_time,
             actual_segment_end_time - previous_segment_end_time, kNoRepeat});
      }
      return;
    }

    // A gap since previous.
    const uint64_t kRoundingErrorGrace = 5;
    if (previous_segment_end_time + kRoundingErrorGrace < start_time) {
      LOG(WARNING) << "Found a gap of size "
                   << (start_time - previous_segment_end_time)
                   << " > kRoundingErrorGrace (" << kRoundingErrorGrace
                   << "). The new segment starts at " << start_time
                   << " but the previous segment ends at "
                   << previous_segment_end_time << ".";
    }

    // No overlapping segments.
    if (start_time < previous_segment_end_time - kRoundingErrorGrace) {
      LOG(WARNING)
          << "Segments should not be overlapping. The new segment starts at "
          << start_time << " but the previous segment ends at "
          << previous_segment_end_time << ".";
    }
  }

  segment_infos_.push_back({start_time, adjusted_duration, kNoRepeat});
}

bool Representation::ApproximiatelyEqual(uint64_t time1, uint64_t time2) const {
  if (!allow_approximate_segment_timeline_)
    return time1 == time2;

  // It is not always possible to align segment duration to target duration
  // exactly. For example, for AAC with sampling rate of 44100, there are always
  // 1024 audio samples per frame, so the frame duration is 1024/44100. For a
  // target duration of 2 seconds, the closest segment duration would be 1.984
  // or 2.00533.

  // An arbitrary error threshold cap. This makes sure that the error is not too
  // large for large samples.
  const double kErrorThresholdSeconds = 0.05;

  // So we consider two times equal if they differ by less than one sample.
  const uint32_t error_threshold =
      std::min(frame_duration_,
               static_cast<uint32_t>(kErrorThresholdSeconds *
                                     media_info_.reference_time_scale()));
  return time1 < time2 + error_threshold && time2 < time1 + error_threshold;
}

uint64_t Representation::AdjustDuration(uint64_t duration) const {
  if (!allow_approximate_segment_timeline_)
    return duration;
  const uint64_t scaled_target_duration =
      mpd_options_.target_segment_duration * media_info_.reference_time_scale();
  return ApproximiatelyEqual(scaled_target_duration, duration)
             ? scaled_target_duration
             : duration;
}

void Representation::SlideWindow() {
  DCHECK(!segment_infos_.empty());
  if (mpd_options_.mpd_params.time_shift_buffer_depth <= 0.0 ||
      mpd_options_.mpd_type == MpdType::kStatic)
    return;

  const uint32_t time_scale = GetTimeScale(media_info_);
  DCHECK_GT(time_scale, 0u);

  uint64_t time_shift_buffer_depth = static_cast<uint64_t>(
      mpd_options_.mpd_params.time_shift_buffer_depth * time_scale);

  // The start time of the latest segment is considered the current_play_time,
  // and this should guarantee that the latest segment will stay in the list.
  const uint64_t current_play_time = LatestSegmentStartTime(segment_infos_);
  if (current_play_time <= time_shift_buffer_depth)
    return;

  const uint64_t timeshift_limit = current_play_time - time_shift_buffer_depth;

  // First remove all the SegmentInfos that are completely out of range, by
  // looking at the very last segment's end time.
  std::list<SegmentInfo>::iterator first = segment_infos_.begin();
  std::list<SegmentInfo>::iterator last = first;
  for (; last != segment_infos_.end(); ++last) {
    const uint64_t last_segment_end_time = LastSegmentEndTime(*last);
    if (timeshift_limit < last_segment_end_time)
      break;
    RemoveSegments(last->start_time, last->duration, last->repeat + 1);
    start_number_ += last->repeat + 1;
  }
  segment_infos_.erase(first, last);

  // Now some segment in the first SegmentInfo should be left in the list.
  SegmentInfo* first_segment_info = &segment_infos_.front();
  DCHECK_LE(timeshift_limit, LastSegmentEndTime(*first_segment_info));

  // Identify which segments should still be in the SegmentInfo.
  const uint64_t repeat_index =
      SearchTimedOutRepeatIndex(timeshift_limit, *first_segment_info);
  if (repeat_index == 0)
    return;

  RemoveSegments(first_segment_info->start_time, first_segment_info->duration,
                 repeat_index);

  first_segment_info->start_time = first_segment_info->start_time +
                                   first_segment_info->duration * repeat_index;
  first_segment_info->repeat = first_segment_info->repeat - repeat_index;
  start_number_ += repeat_index;
}

void Representation::RemoveSegments(uint64_t start_time,
                                    uint64_t duration,
                                    uint64_t num_segments) {
  if (mpd_options_.mpd_params.preserved_segments_outside_live_window == 0)
    return;

  for (size_t i = 0; i < num_segments; ++i) {
    segments_to_be_removed_.push_back(media::GetSegmentName(
        media_info_.segment_template(), start_time + i * duration,
        start_number_ - 1 + i, media_info_.bandwidth()));
  }
  while (segments_to_be_removed_.size() >
         mpd_options_.mpd_params.preserved_segments_outside_live_window) {
    VLOG(2) << "Deleting " << segments_to_be_removed_.front();
    File::Delete(segments_to_be_removed_.front().c_str());
    segments_to_be_removed_.pop_front();
  }
}

std::string Representation::GetVideoMimeType() const {
  return GetMimeType("video", media_info_.container_type());
}

std::string Representation::GetAudioMimeType() const {
  return GetMimeType("audio", media_info_.container_type());
}

std::string Representation::GetTextMimeType() const {
  CHECK(media_info_.has_text_info());
  if (media_info_.text_info().codec() == "ttml") {
    switch (media_info_.container_type()) {
      case MediaInfo::CONTAINER_TEXT:
        return "application/ttml+xml";
      case MediaInfo::CONTAINER_MP4:
        return "application/mp4";
      default:
        LOG(ERROR) << "Failed to determine MIME type for TTML container: "
                   << media_info_.container_type();
        return "";
    }
  }
  if (media_info_.text_info().codec() == "wvtt") {
    if (media_info_.container_type() == MediaInfo::CONTAINER_TEXT) {
      return "text/vtt";
    } else if (media_info_.container_type() == MediaInfo::CONTAINER_MP4) {
      return "application/mp4";
    }
    LOG(ERROR) << "Failed to determine MIME type for VTT container: "
               << media_info_.container_type();
    return "";
  }

  LOG(ERROR) << "Cannot determine MIME type for format: "
             << media_info_.text_info().codec()
             << " container: " << media_info_.container_type();
  return "";
}

}  // namespace shaka