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

#include "mf_encoder_factory.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <map>
#include <mutex>
#include <string>

#include "mf_h264_encoder_impl.h"
#include "rtc_base/logging.h"

namespace webrtc {

namespace {

// Probe once per process: MFStartup + enumerate hardware NV12->H264
// encoder MFTs. MFShutdown is intentionally never called — Media
// Foundation stays initialized for the process lifetime (the cost is a
// few handles; tearing it down while encoders exist is far riskier).
bool ProbeHardwareH264Mft() {
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFStartup failed (hr=0x" << std::hex << hr
                        << "); Media Foundation encoding unavailable.";
    return false;
  }

  MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                 &input_info, &output_info, &activates, &count);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFTEnumEx failed (hr=0x" << std::hex << hr << ")";
    return false;
  }

  if (count > 0) {
    WCHAR name_buf[256] = {};
    UINT32 name_len = 0;
    if (SUCCEEDED(activates[0]->GetString(MFT_FRIENDLY_NAME_Attribute,
                                          name_buf, ARRAYSIZE(name_buf),
                                          &name_len))) {
      RTC_LOG(LS_INFO) << "Media Foundation hardware H264 encoder available ("
                       << count << " MFT(s)).";
    }
  } else {
    RTC_LOG(LS_WARNING)
        << "No hardware H264 encoder MFT found (VM/RDP/old driver?); "
           "Media Foundation encoding unavailable.";
  }

  for (UINT32 i = 0; i < count; i++) {
    activates[i]->Release();
  }
  if (activates) {
    CoTaskMemFree(activates);
  }
  return count > 0;
}

}  // namespace

MediaFoundationVideoEncoderFactory::MediaFoundationVideoEncoderFactory() {
  // Same H264 flavor the SDK's sender path prefers (constrained baseline,
  // packetization-mode 1) and that the NVENC factory advertises.
  std::map<std::string, std::string> baseline_parameters = {
      {"profile-level-id", "42e01f"},
      {"level-asymmetry-allowed", "1"},
      {"packetization-mode", "1"},
  };
  supported_formats_.push_back(SdpVideoFormat("H264", baseline_parameters));
}

MediaFoundationVideoEncoderFactory::~MediaFoundationVideoEncoderFactory() =
    default;

bool MediaFoundationVideoEncoderFactory::IsSupported() {
  static const bool supported = ProbeHardwareH264Mft();
  return supported;
}

std::unique_ptr<VideoEncoder> MediaFoundationVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  for (const auto& supported_format : supported_formats_) {
    if (format.IsSameCodec(supported_format)) {
      if (format.name == "H264") {
        RTC_LOG(LS_INFO)
            << "Using Media Foundation hardware encoder for H264";
        return std::make_unique<MediaFoundationH264EncoderImpl>(env, format);
      }
    }
  }
  return nullptr;
}

std::vector<SdpVideoFormat>
MediaFoundationVideoEncoderFactory::GetSupportedFormats() const {
  return supported_formats_;
}

std::vector<SdpVideoFormat>
MediaFoundationVideoEncoderFactory::GetImplementations() const {
  return supported_formats_;
}

}  // namespace webrtc
