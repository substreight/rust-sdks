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
#include <objbase.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "api/video_codecs/video_encoder_software_fallback_wrapper.h"
#include "mf_h264_encoder_impl.h"
#include "rtc_base/logging.h"
#if defined(WEBRTC_USE_H264)
#include "modules/video_coding/codecs/h264/include/h264.h"
#endif

namespace webrtc {

namespace {

// RTC_LOG's stream does not understand std::hex; format HRESULTs by hand.
std::string HexHr(HRESULT hr) {
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

// Probe once per process: MFStartup + enumerate hardware NV12->H264
// encoder MFTs. MFShutdown is intentionally never called — Media
// Foundation stays initialized for the process lifetime (the cost is a
// few handles; tearing it down while encoders exist is far riskier).
bool ProbeHardwareH264Mft() {
  // MF enumeration/activation is COM; the probing thread may not have an
  // apartment. Join the MTA (leaked deliberately; see encoder impl).
  thread_local const HRESULT com_init =
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)com_init;

  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFStartup failed (hr=" << HexHr(hr)
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
    RTC_LOG(LS_WARNING) << "MFTEnumEx failed (hr=" << HexHr(hr) << ")";
    return false;
  }

  if (count > 0) {
    RTC_LOG(LS_INFO) << "Media Foundation hardware H264 encoder available ("
                     << count << " MFT(s)).";
    for (UINT32 i = 0; i < count; i++) {
      WCHAR name_buf[256] = {};
      UINT32 name_len = 0;
      if (SUCCEEDED(activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute,
                                            name_buf, ARRAYSIZE(name_buf),
                                            &name_len))) {
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, name_buf, name_len,
                                           nullptr, 0, nullptr, nullptr);
        std::string utf8(utf8_len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, name_buf, name_len, utf8.data(),
                            utf8_len, nullptr, nullptr);
        RTC_LOG(LS_INFO) << "  MFT[" << i << "]: \"" << utf8 << "\"";
      }
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
  // High profile first: CABAC + 8x8 transforms are ~10% better text/UI
  // quality at the same bitrate, every hardware MFT we target supports it,
  // and 640032 is the exact High fmtp LiveKit's SFU registers (the SFU
  // strips High from SUBSCRIBER offers but forwards the bitstream over the
  // baseline-signaled PT, so no viewer-side work is needed).
  std::map<std::string, std::string> high_parameters = {
      {"profile-level-id", "640032"},
      {"level-asymmetry-allowed", "1"},
      {"packetization-mode", "1"},
  };
  supported_formats_.push_back(SdpVideoFormat("H264", high_parameters));
  // Constrained baseline fallback: same flavor the SDK's sender path
  // historically preferred and that the NVENC factory advertises.
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
        auto hw = std::make_unique<MediaFoundationH264EncoderImpl>(env, format);
#if defined(WEBRTC_USE_H264)
        // Wrap with the software fallback: if the hardware MFT fails to
        // initialize or signals WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE mid
        // stream, viewers get OpenH264 video instead of nothing. Without
        // this, a pinned-Hardware encoder selector retries the broken HW
        // encoder forever (observed in the field: 0 frames encoded, PLI
        // storm, black stream).
        RTC_LOG(LS_INFO) << "Using Media Foundation hardware encoder for "
                            "H264 (OpenH264 fallback armed)";
        return CreateVideoEncoderSoftwareFallbackWrapper(
            env, CreateH264Encoder(env, H264EncoderSettings::Parse(format)),
            std::move(hw), /*prefer_temporal_support=*/false);
#else
        RTC_LOG(LS_INFO)
            << "Using Media Foundation hardware encoder for H264";
        return hw;
#endif
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
