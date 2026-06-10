/*
 * Copyright 2026 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef WEBRTC_MF_H264_ENCODER_IMPL_H_
#define WEBRTC_MF_H264_ENCODER_IMPL_H_

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "api/environment/environment.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "modules/video_coding/codecs/h264/include/h264.h"

namespace webrtc {

// webrtc::VideoEncoder over a Windows Media Foundation hardware H.264
// encoder MFT (NVENC / AMF / QuickSync). Iteration 1 design:
//   - CPU NV12 input samples (libyuv I420->NV12); no D3D11 textures.
//   - The async MFT is driven synchronously from Encode() with
//     credit-banking: METransformNeedInput credits are consumed
//     immediately when input is pending, banked otherwise;
//     METransformHaveOutput is drained opportunistically. Output for
//     frame N is typically delivered during Encode(N) or Encode(N+1)
//     (<= one frame interval of added latency).
class MediaFoundationH264EncoderImpl : public VideoEncoder {
 public:
  MediaFoundationH264EncoderImpl(const Environment& env,
                                 const SdpVideoFormat& format);
  ~MediaFoundationH264EncoderImpl() override;

  int32_t InitEncode(const VideoCodec* codec_settings,
                     const Settings& settings) override;
  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;
  int32_t Release() override;
  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;
  void SetRates(const RateControlParameters& rc_parameters) override;
  EncoderInfo GetEncoderInfo() const override;

 private:
  struct PendingFrameInfo {
    int64_t sample_time_100ns;
    uint32_t rtp_timestamp;
    int64_t ntp_time_ms;
    int64_t render_time_ms;
    VideoRotation rotation;
  };

  // Activate the first hardware NV12->H264 encoder MFT and configure
  // media types, low-latency CodecAPI properties and rate control.
  int32_t CreateAndConfigureTransform();
  // Pump the MFT event queue. If `wait_for_input_credit` is set, blocks
  // (bounded) until the MFT grants an input credit.
  int32_t PumpEvents(bool wait_for_input_credit);
  int32_t DeliverPendingInput();
  int32_t DrainOneOutput();
  HRESULT ApplyBitrate(uint32_t bitrate_bps);
  int32_t ProcessOutputSample(Microsoft::WRL::ComPtr<IMFSample> sample);

  const Environment& env_;
  const SdpVideoFormat format_;
  EncodedImageCallback* encoded_image_callback_ = nullptr;

  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_;
  std::string mft_friendly_name_;
  DWORD input_stream_id_ = 0;
  DWORD output_stream_id_ = 0;
  bool output_provides_samples_ = true;
  DWORD output_buffer_size_ = 0;

  // Credits granted by METransformNeedInput not yet consumed.
  int input_credits_ = 0;
  // NV12 sample awaiting an input credit.
  Microsoft::WRL::ComPtr<IMFSample> pending_input_;
  std::deque<PendingFrameInfo> pending_frames_;

  std::vector<uint8_t> nv12_scratch_;

  VideoCodec codec_;
  uint32_t configured_bitrate_bps_ = 0;
  uint32_t target_bitrate_bps_ = 0;
  int64_t last_bitrate_update_ms_ = 0;
  bool force_key_frame_ = false;
  bool sending_ = true;

  EncodedImage encoded_image_;
  H264BitstreamParser h264_bitstream_parser_;
  bool initialized_ = false;
};

}  // namespace webrtc

#endif  // WEBRTC_MF_H264_ENCODER_IMPL_H_
