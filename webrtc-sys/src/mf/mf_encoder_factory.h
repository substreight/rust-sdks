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

#ifndef WEBRTC_MF_ENCODER_FACTORY_H_
#define WEBRTC_MF_ENCODER_FACTORY_H_

#include <memory>
#include <vector>

#include "api/environment/environment.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace webrtc {

// Hardware H.264 encoding on Windows via Media Foundation hardware MFTs
// (NVENC / AMD AMF / Intel QuickSync all surface as H.264 encoder MFTs).
class MediaFoundationVideoEncoderFactory : public VideoEncoderFactory {
 public:
  MediaFoundationVideoEncoderFactory();
  ~MediaFoundationVideoEncoderFactory() override;

  // True when at least one hardware H.264 encoder MFT (NV12 in, H264 out)
  // is present. Result is probed once and cached for the process lifetime.
  static bool IsSupported();

  std::unique_ptr<VideoEncoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override;

  std::vector<SdpVideoFormat> GetSupportedFormats() const override;
  std::vector<SdpVideoFormat> GetImplementations() const override;

 private:
  std::vector<SdpVideoFormat> supported_formats_;
};

}  // namespace webrtc

#endif  // WEBRTC_MF_ENCODER_FACTORY_H_
