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

#include "mf_h264_encoder_impl.h"

// initguid must precede codecapi.h in exactly one TU so the CODECAPI_*
// GUIDs are instantiated instead of left as extern declarations.
#include <initguid.h>

#include <codecapi.h>
#include <d3d11_4.h>
#include <mferror.h>
#include <objbase.h>

#include <algorithm>
#include <utility>

#include "api/video_codecs/h264_profile_level_id.h"
#include "common_video/h264/h264_common.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "mf_encoder_factory.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"

using Microsoft::WRL::ComPtr;

namespace webrtc {

namespace {

// Upper bound on a single synchronous wait for the MFT to grant an input
// credit or produce output. Hitting this repeatedly means the hardware
// session is wedged; we surface an encoder error so libwebrtc re-creates
// the encoder (and the factory can fall back to software).
constexpr int kPumpDeadlineMs = 250;
// Cold hardware sessions need far longer to grant their FIRST credit (Intel
// QSV first-ever activation has been measured in seconds; AMD warmup too).
// A tight first deadline would dump perfectly good encoders to software.
constexpr int kFirstPumpDeadlineMs = 2500;
// Rebuild sessions (resolution change, InitEncode re-init) run on a warm
// driver stack but a brand-new MFT session can still take longer than the
// steady-state deadline to grant its first credit, especially under GPU
// contention (game streaming). Waiting ~1s is far better than dumping a
// healthy hardware session to the OpenH264 fallback.
constexpr int kRebuildPumpDeadlineMs = 1000;

// Re-apply dynamic bitrate at most once a second; some vendor MFTs glitch
// when CodecAPI values are hammered every SetRates() call (~30/s).
constexpr int64_t kBitrateUpdateIntervalMs = 1000;

HRESULT SetCodecApiU32(ICodecAPI* api, const GUID& guid, UINT32 value) {
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_UI4;
  var.ulVal = value;
  return api->SetValue(&guid, &var);
}

// Boolean CodecAPI properties are documented as VARIANT_BOOL; strict
// vendors (Intel/Qualcomm HMFTs) reject a VT_UI4 in their place.
HRESULT SetCodecApiBool(ICodecAPI* api, const GUID& guid, bool value) {
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_BOOL;
  var.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return api->SetValue(&guid, &var);
}

// HRD/VBV size in bits: ~0.75s of the target rate. Big enough that the
// rate controller averages across IDRs instead of starving the frames
// after each one; small enough that an IDR burst can't add a second of
// pacer latency. Capped for the 4K tiers.
UINT32 HrdBufferBitsFor(uint32_t bitrate_bps) {
  return static_cast<UINT32>(
      std::min<uint64_t>(uint64_t{bitrate_bps} * 3 / 4, 60'000'000));
}

// RTC_LOG's stream does not understand std::hex (it prints the manipulator's
// function pointer); format HRESULTs by hand.
std::string HexHr(HRESULT hr) {
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

std::string GetActivateFriendlyName(IMFActivate* activate) {
  WCHAR name_buf[256] = {};
  UINT32 name_len = 0;
  if (FAILED(activate->GetString(MFT_FRIENDLY_NAME_Attribute, name_buf,
                                 ARRAYSIZE(name_buf), &name_len))) {
    return "<unnamed MFT>";
  }
  int utf8_len = WideCharToMultiByte(CP_UTF8, 0, name_buf, name_len, nullptr,
                                     0, nullptr, nullptr);
  std::string utf8(utf8_len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, name_buf, name_len, utf8.data(), utf8_len,
                      nullptr, nullptr);
  return utf8;
}

}  // namespace

MediaFoundationH264EncoderImpl::MediaFoundationH264EncoderImpl(
    const Environment& env,
    const SdpVideoFormat& format)
    : env_(env), format_(format) {
  // The factory advertises both Constrained Baseline (42e01f) and High
  // (640032); encode whichever profile this format negotiated.
  const auto profile_level = ParseSdpForH264ProfileLevelId(format_.parameters);
  high_profile_ =
      profile_level &&
      (profile_level->profile == H264Profile::kProfileHigh ||
       profile_level->profile == H264Profile::kProfileConstrainedHigh ||
       profile_level->profile == H264Profile::kProfilePredictiveHigh444);
}

MediaFoundationH264EncoderImpl::~MediaFoundationH264EncoderImpl() {
  Release();
  // Release() deliberately preserves these across re-inits; the destructor
  // is where they actually die.
  cached_activate_.Reset();
  dxgi_manager_.Reset();
  d3d_device_.Reset();
}

int32_t MediaFoundationH264EncoderImpl::InitEncode(
    const VideoCodec* inst,
    const VideoEncoder::Settings& settings) {
  if (!inst || inst->codecType != kVideoCodecH264) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->maxFramerate == 0 || inst->width < 1 || inst->height < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  int32_t release_ret = Release();
  if (release_ret != WEBRTC_VIDEO_CODEC_OK) {
    return release_ret;
  }

  codec_ = *inst;
  if (codec_.numberOfSimulcastStreams == 0) {
    codec_.simulcastStream[0].width = codec_.width;
    codec_.simulcastStream[0].height = codec_.height;
  }

  target_bitrate_bps_ = codec_.startBitrate * 1000;
  if (target_bitrate_bps_ == 0) {
    target_bitrate_bps_ = codec_.maxBitrate * 1000;
  }

  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  encoded_image_.set_size(0);

  int32_t ret = CreateAndConfigureTransform();
  if (ret != WEBRTC_VIDEO_CODEC_OK) {
    Release();
    return ret;
  }

  nv12_scratch_.clear();
  pending_frames_.clear();
  input_credits_ = 0;
  pending_input_.Reset();
  // A rate re-init queued against the PREVIOUS session is moot: this fresh
  // session is already configured at the new target. A stale flag would
  // flush/retype the brand-new transform on its first Encode for nothing.
  pending_rate_reinit_ = false;
  force_key_frame_ = true;  // first frame must be an IDR
  sending_ = true;
  initialized_ = true;

  RTC_LOG(LS_INFO) << "MediaFoundation H264 encoder initialized: "
                   << mft_friendly_name_ << " " << codec_.width << "x"
                   << codec_.height << " @ " << codec_.maxFramerate
                   << "fps, target_bps=" << target_bitrate_bps_;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::CreateAndConfigureTransform() {
  // libwebrtc's encoder task-queue thread has no COM apartment, and
  // IMFActivate::ActivateObject is COM object creation — without this the
  // hardware MFT activates fine in probes (main thread) but fails here in
  // production. Join the MTA once per thread; deliberately never
  // uninitialized (encoder threads are long-lived). S_FALSE and
  // RPC_E_CHANGED_MODE both leave COM usable.
  thread_local const HRESULT com_init =
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  (void)com_init;

  if (!MediaFoundationVideoEncoderFactory::IsSupported()) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // This is also the REBUILD path (mid-stream resolution changes). Tear the
  // old session down and reset all per-session pump state first: credits or
  // pending frames leaking from the dead transform make the fresh MFT
  // reject its first ProcessInput (MF_E_NOTACCEPTING) and silently dump the
  // stream to the OpenH264 fallback.
  ResetTransformState();

  // Fast path: re-activate the MFT that configured successfully last
  // session instead of re-running the MFTEnum2/DXGI sweep. ShutdownObject
  // first — IMFActivate caches its created object, and we want a fresh
  // transform, not the one we just end-streamed.
  if (cached_activate_) {
    cached_activate_->ShutdownObject();
    if (TryConfigureTransform(cached_activate_.Get(), cached_activate_name_,
                              cached_activate_has_luid_
                                  ? &cached_activate_luid_
                                  : nullptr) == WEBRTC_VIDEO_CODEC_OK) {
      mft_friendly_name_ = cached_activate_name_;
      return WEBRTC_VIDEO_CODEC_OK;
    }
    RTC_LOG(LS_WARNING) << "MF encoder: cached MFT \"" << cached_activate_name_
                        << "\" no longer configures; re-enumerating.";
    ResetTransformState();
    cached_activate_.Reset();
    cached_activate_has_luid_ = false;
    cached_activate_name_.clear();
  }

  MFT_REGISTER_TYPE_INFO input_info = {MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_info = {MFMediaType_Video, MFVideoFormat_H264};

  // Hardware MFTs are registered PER GPU ADAPTER, and a D3D device manager
  // is only accepted by an MFT whose adapter matches the device's. Enumerate
  // per adapter via MFTEnum2 + MFT_ENUM_ADAPTER_LUID (Chromium's approach)
  // so every candidate is paired with the adapter its device must live on.
  struct Candidate {
    ComPtr<IMFActivate> activate;
    LUID luid{};
    bool has_luid = false;
  };
  std::vector<Candidate> candidates;

  ComPtr<IDXGIFactory1> dxgi_factory;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory)))) {
    for (UINT i = 0;; i++) {
      ComPtr<IDXGIAdapter1> adapter;
      if (dxgi_factory->EnumAdapters1(i, &adapter) != S_OK) {
        break;
      }
      DXGI_ADAPTER_DESC1 desc{};
      if (FAILED(adapter->GetDesc1(&desc)) ||
          (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        continue;
      }

      ComPtr<IMFAttributes> attrs;
      if (FAILED(MFCreateAttributes(&attrs, 1))) {
        continue;
      }
      attrs->SetBlob(MFT_ENUM_ADAPTER_LUID,
                     reinterpret_cast<const UINT8*>(&desc.AdapterLuid),
                     sizeof(LUID));
      IMFActivate** activates = nullptr;
      UINT32 count = 0;
      if (FAILED(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER,
                          MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                          &input_info, &output_info, attrs.Get(), &activates,
                          &count))) {
        continue;
      }
      for (UINT32 j = 0; j < count; j++) {
        Candidate c;
        c.activate.Attach(activates[j]);  // take ownership of the ref
        c.luid = desc.AdapterLuid;
        c.has_luid = true;
        candidates.push_back(std::move(c));
      }
      if (activates) {
        CoTaskMemFree(activates);
      }
    }
  }

  // Fallback (pre-1703 Windows or DXGI failure): flat enumeration with no
  // adapter info — candidates run in system-memory mode, no D3D manager.
  if (candidates.empty()) {
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr =
        MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                  MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                  &input_info, &output_info, &activates, &count);
    if (FAILED(hr) || count == 0) {
      RTC_LOG(LS_ERROR) << "No hardware H264 encoder MFT found (hr="
                        << HexHr(hr) << ")";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    for (UINT32 j = 0; j < count; j++) {
      Candidate c;
      c.activate.Attach(activates[j]);
      candidates.push_back(std::move(c));
    }
    if (activates) {
      CoTaskMemFree(activates);
    }
  }

  RTC_LOG(LS_INFO) << "MF encoder: " << candidates.size()
                   << " hardware H264 MFT candidate(s)";
  int32_t result = WEBRTC_VIDEO_CODEC_ERROR;
  for (auto& candidate : candidates) {
    std::string name = GetActivateFriendlyName(candidate.activate.Get());
    RTC_LOG(LS_INFO) << "MF encoder: trying MFT \"" << name << "\""
                     << (candidate.has_luid ? " (adapter-matched)" : "");
    if (TryConfigureTransform(candidate.activate.Get(), name,
                              candidate.has_luid ? &candidate.luid
                                                 : nullptr) ==
        WEBRTC_VIDEO_CODEC_OK) {
      mft_friendly_name_ = name;
      // Remember the winner so rebuilds skip the enumeration sweep.
      cached_activate_ = candidate.activate;
      cached_activate_luid_ = candidate.luid;
      cached_activate_has_luid_ = candidate.has_luid;
      cached_activate_name_ = name;
      result = WEBRTC_VIDEO_CODEC_OK;
      break;
    }
    ResetTransformState();
  }

  if (result != WEBRTC_VIDEO_CODEC_OK) {
    RTC_LOG(LS_ERROR)
        << "MF encoder: no hardware MFT accepted our configuration";
  }
  return result;
}

bool MediaFoundationH264EncoderImpl::EnsureDeviceManagerForAdapter(
    const LUID& adapter_luid) {
  if (dxgi_manager_ && dxgi_manager_luid_.HighPart == adapter_luid.HighPart &&
      dxgi_manager_luid_.LowPart == adapter_luid.LowPart) {
    // Health-check the cached device: after a GPU timeout/TDR the driver
    // can recover under the SAME LUID with our device in the REMOVED state.
    // Reusing it would fail every candidate's type negotiation and pin the
    // session to the software fallback for the encoder's lifetime.
    if (d3d_device_ && d3d_device_->GetDeviceRemovedReason() == S_OK) {
      return true;
    }
    RTC_LOG(LS_WARNING) << "MF encoder: cached D3D device was removed "
                           "(TDR/driver reset); recreating";
  }
  dxgi_manager_.Reset();
  d3d_device_.Reset();

  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return false;
  }
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        desc.AdapterLuid.HighPart == adapter_luid.HighPart &&
        desc.AdapterLuid.LowPart == adapter_luid.LowPart) {
      break;
    }
    adapter.Reset();
  }
  if (!adapter) {
    return false;
  }

  ComPtr<ID3D11Device> device;
  static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1,
                                              D3D_FEATURE_LEVEL_11_0};
  // An explicit adapter requires D3D_DRIVER_TYPE_UNKNOWN.
  HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 kLevels, ARRAYSIZE(kLevels), D3D11_SDK_VERSION,
                                 &device, nullptr, nullptr);
  if (FAILED(hr) || !device) {
    RTC_LOG(LS_WARNING) << "MF encoder: D3D11CreateDevice on MFT adapter "
                           "failed (hr="
                        << HexHr(hr) << ")";
    return false;
  }
  ComPtr<ID3D11Multithread> multithread;
  if (SUCCEEDED(device.As(&multithread)) && multithread) {
    multithread->SetMultithreadProtected(TRUE);
  }
  UINT reset_token = 0;
  if (FAILED(MFCreateDXGIDeviceManager(&reset_token, &dxgi_manager_)) ||
      !dxgi_manager_ ||
      FAILED(dxgi_manager_->ResetDevice(device.Get(), reset_token))) {
    dxgi_manager_.Reset();
    return false;
  }
  d3d_device_ = device;
  dxgi_manager_luid_ = adapter_luid;
  return true;
}

int32_t MediaFoundationH264EncoderImpl::TryConfigureTransform(
    IMFActivate* activate,
    const std::string& name,
    const LUID* adapter_luid) {
  HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&transform_));
  if (FAILED(hr) || !transform_) {
    RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": ActivateObject failed (hr="
                        << HexHr(hr) << ")";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Hardware encoder MFTs are async: unlock them and request low latency.
  ComPtr<IMFAttributes> attributes;
  bool d3d_aware = false;
  if (SUCCEEDED(transform_->GetAttributes(&attributes)) && attributes) {
    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    UINT32 aware = 0;
    if (SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware))) {
      d3d_aware = aware != 0;
    }
  }

  // Attach a DXGI device manager BEFORE negotiating media types — but ONLY
  // one whose device lives on the MFT's own adapter (cross-adapter managers
  // are rejected with E_FAIL), and only to MFTs declaring D3D11 awareness.
  if (d3d_aware && adapter_luid &&
      EnsureDeviceManagerForAdapter(*adapter_luid)) {
    HRESULT mhr = transform_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(dxgi_manager_.Get()));
    if (SUCCEEDED(mhr)) {
      d3d_manager_attached_ = true;
      RTC_LOG(LS_INFO) << "MFT \"" << name
                       << "\": adapter-matched DXGI device manager attached";
    } else {
      RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": rejected D3D manager "
                             "(hr=" << HexHr(mhr) << "); continuing without";
    }
  } else if (!d3d_aware) {
    RTC_LOG(LS_INFO) << "MFT \"" << name << "\": not D3D11-aware";
  }

  hr = transform_.As(&event_generator_);
  if (FAILED(hr) || !event_generator_) {
    RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": not an event generator "
                           "(not async?); skipping.";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  DWORD input_ids[1] = {};
  DWORD output_ids[1] = {};
  hr = transform_->GetStreamIDs(1, input_ids, 1, output_ids);
  if (SUCCEEDED(hr)) {
    input_stream_id_ = input_ids[0];
    output_stream_id_ = output_ids[0];
  } else {
    // E_NOTIMPL => fixed streams, IDs are 0.
    input_stream_id_ = 0;
    output_stream_id_ = 0;
  }

  // Rate control + latency knobs (best effort; vendors vary).
  if (FAILED(transform_.As(&codec_api_)) || !codec_api_) {
    codec_api_.Reset();
    RTC_LOG(LS_WARNING) << "MFT \"" << name
                        << "\": no ICodecAPI; using type defaults.";
  }
  ApplyCodecApiKnobs(name);

  int32_t types_ret = ConfigureMediaTypes(name);
  if (types_ret != WEBRTC_VIDEO_CODEC_OK) {
    return types_ret;
  }

  hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": NOTIFY_BEGIN_STREAMING "
                           "failed (hr=" << HexHr(hr) << ")";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  return WEBRTC_VIDEO_CODEC_OK;
}

void MediaFoundationH264EncoderImpl::ApplyCodecApiKnobs(
    const std::string& name) {
  if (!codec_api_) {
    return;
  }
  // Every rejection is logged: a silently-ignored knob (B-frames, VBR) is
  // the difference between smooth and weird on some driver stacks.
  auto set_knob = [&](const GUID& guid, UINT32 value, const char* knob) {
    HRESULT rc = SetCodecApiU32(codec_api_.Get(), guid, value);
    if (FAILED(rc)) {
      RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": rejected " << knob
                          << " (hr=" << HexHr(rc) << ")";
    }
  };
  set_knob(CODECAPI_AVEncCommonRateControlMode,
           eAVEncCommonRateControlMode_CBR, "CBR rate control");
  set_knob(CODECAPI_AVEncCommonMeanBitRate, target_bitrate_bps_,
           "mean bitrate");
  // Without an HRD/VBV buffer the encoder budgets frame-by-frame, so every
  // IDR starves the frames after it (periodic blur pulse) and the pacer
  // bursts. ~0.75s of target averages rate across the keyframe.
  set_knob(CODECAPI_AVEncCommonBufferSize,
           HrdBufferBitsFor(target_bitrate_bps_), "HRD buffer size");
  set_knob(CODECAPI_AVLowLatencyMode, TRUE, "low-latency mode");
  set_knob(CODECAPI_AVEncMPVDefaultBPictureCount, 0, "B-frame disable");
  // Keyframes ~1/min: steady-state recovery is PLI-driven (the SFU
  // throttles PLIs), and a periodic IDR inside a CBR budget is a
  // metronomic quality dip. 10s GOPs were the old behavior and visibly
  // pulsed; Discord ships ~1/min for the same reason.
  set_knob(CODECAPI_AVEncMPVGOPSize, codec_.maxFramerate * 60, "GOP size");
  // Hard floor under perceptual quality: prefer briefly dropping frames
  // over QP mud when the budget is exceeded.
  set_knob(CODECAPI_AVEncVideoMaxQP, 40, "max QP");
  if (high_profile_) {
    // High alone doesn't guarantee CABAC on all vendors; ask explicitly.
    // VT_BOOL, not VT_UI4 — strict vendors reject the wrong VARIANT type.
    HRESULT rc =
        SetCodecApiBool(codec_api_.Get(), CODECAPI_AVEncH264CABACEnable, true);
    if (FAILED(rc)) {
      RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": rejected CABAC (hr="
                          << HexHr(rc) << ")";
    }
  }
  configured_bitrate_bps_ = target_bitrate_bps_;
}

int32_t MediaFoundationH264EncoderImpl::ConfigureMediaTypes(
    const std::string& name) {
  // Output type FIRST (encoder MFTs negotiate input against output).
  ComPtr<IMFMediaType> output_type;
  HRESULT hr = MFCreateMediaType(&output_type);
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  output_type->SetUINT32(MF_MT_AVG_BITRATE, target_bitrate_bps_);
  MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, codec_.width,
                     codec_.height);
  MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, codec_.maxFramerate,
                      1);
  MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  // Profile follows the negotiated SDP format: High (640032, CABAC/8x8 —
  // ~10% better text/UI quality at the same bitrate) when offered and
  // accepted, Constrained-ish Baseline (42e01f) otherwise. B frames are
  // disabled via CodecAPI either way.
  output_type->SetUINT32(MF_MT_MPEG2_PROFILE, high_profile_
                                                  ? eAVEncH264VProfile_High
                                                  : eAVEncH264VProfile_Base);
  hr = transform_->SetOutputType(output_stream_id_, output_type.Get(), 0);
  if (hr == MF_E_UNSUPPORTED_D3D_TYPE && d3d_manager_attached_) {
    // Documented contract for D3D-aware MFTs: on MF_E_UNSUPPORTED_D3D_TYPE,
    // detach the manager (SET_D3D_MANAGER with NULL) so the MFT reverts to
    // system-memory intake, then retry. Leaving a half-set manager in place
    // keeps every subsequent type negotiation failing.
    RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": D3D type rejected; "
                           "reverting to system-memory mode and retrying";
    transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    d3d_manager_attached_ = false;
    hr = transform_->SetOutputType(output_stream_id_, output_type.Get(), 0);
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": SetOutputType(H264) "
                           "failed (hr=" << HexHr(hr) << ")";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  ComPtr<IMFMediaType> input_type;
  hr = MFCreateMediaType(&input_type);
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, codec_.width,
                     codec_.height);
  MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, codec_.maxFramerate,
                      1);
  MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  input_type->SetUINT32(MF_MT_DEFAULT_STRIDE, codec_.width);
  hr = transform_->SetInputType(input_stream_id_, input_type.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFT \"" << name << "\": SetInputType(NV12) "
                           "failed (hr=" << HexHr(hr) << ")";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Honor the stride the MFT actually negotiated — some vendors (notably
  // Intel) pad rows past the frame width, and feeding width-stride buffers
  // to a padded-stride type corrupts or rejects frames.
  input_stride_ = codec_.width;
  ComPtr<IMFMediaType> negotiated_input;
  if (SUCCEEDED(transform_->GetInputCurrentType(input_stream_id_,
                                                &negotiated_input)) &&
      negotiated_input) {
    UINT32 stride = 0;
    if (SUCCEEDED(negotiated_input->GetUINT32(MF_MT_DEFAULT_STRIDE,
                                              &stride))) {
      // Negative strides (bottom-up) arrive as huge UINT32 values; only
      // accept sane top-down strides at least as wide as the frame.
      if (static_cast<INT32>(stride) >= static_cast<INT32>(codec_.width)) {
        input_stride_ = stride;
        if (stride != codec_.width) {
          RTC_LOG(LS_INFO) << "MFT \"" << name << "\": padded input stride "
                           << stride << " (width " << codec_.width << ")";
        }
      }
    }
  }

  MFT_OUTPUT_STREAM_INFO stream_info = {};
  if (SUCCEEDED(transform_->GetOutputStreamInfo(output_stream_id_,
                                                &stream_info))) {
    output_provides_samples_ =
        (stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    output_buffer_size_ = stream_info.cbSize;
  }
  if (output_buffer_size_ == 0) {
    // Generous fallback: an IDR at high bitrate fits comfortably.
    output_buffer_size_ =
        static_cast<DWORD>(codec_.width) * codec_.height * 2;
  }
  // MF_MT_AVG_BITRATE above applied the target regardless of ICodecAPI —
  // latch it HERE, not (only) in ApplyCodecApiKnobs, whose null-codec_api_
  // early-return would leave configured_bitrate_bps_ at 0 forever and feed
  // SetRates' drift check a permanent ">=30%" (re-init loop every 5s).
  configured_bitrate_bps_ = target_bitrate_bps_;
  return WEBRTC_VIDEO_CODEC_OK;
}

bool MediaFoundationH264EncoderImpl::TryInPlaceFormatChange() {
  if (!transform_ || !event_generator_) {
    return false;
  }
  // Discard in-flight work — its outputs die with the old format. After a
  // flush an async MFT issues no METransformNeedInput until it sees
  // NOTIFY_START_OF_STREAM, so all pump state resets below.
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  HRESULT hr = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  if (FAILED(hr)) {
    return false;
  }
  pending_input_.Reset();
  pending_frames_.clear();
  input_credits_ = 0;
  got_first_input_credit_ = false;

  // Drain events the MFT queued BEFORE the flush: a flush cannot retract an
  // already-queued METransformNeedInput/HaveOutput, and consuming one after
  // the restart banks a phantom credit (-> MF_E_NOTACCEPTING) or drives
  // ProcessOutput with no pending output (-> E_UNEXPECTED) — either of which
  // would dump this healthy hardware session to the software fallback the
  // moment the format change SUCCEEDED. Race-free here: the async-MFT
  // contract forbids new events between flush and START_OF_STREAM.
  while (true) {
    ComPtr<IMFMediaEvent> stale;
    HRESULT ehr = event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &stale);
    if (ehr == MF_E_NO_EVENTS_AVAILABLE) {
      break;
    }
    if (FAILED(ehr)) {
      return false;  // queue unusable — take the full-rebuild path
    }
  }

  if (ConfigureMediaTypes(mft_friendly_name_) != WEBRTC_VIDEO_CODEC_OK) {
    RTC_LOG(LS_WARNING) << "MFT \"" << mft_friendly_name_
                        << "\": in-place format change rejected; "
                           "falling back to a full rebuild";
    return false;
  }
  // Media-type churn resets rate control on some vendors; re-assert.
  ApplyCodecApiKnobs(mft_friendly_name_);

  hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(hr)) {
    return false;
  }
  RTC_LOG(LS_INFO) << "MFT \"" << mft_friendly_name_
                   << "\": in-place format change to " << codec_.width << "x"
                   << codec_.height;
  return true;
}

void MediaFoundationH264EncoderImpl::ResetTransformState() {
  if (transform_) {
    // Graceful teardown of the live session before dropping our reference;
    // hardware sessions otherwise linger until the MFT object finalizes.
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
  }
  pending_input_.Reset();
  pending_frames_.clear();
  codec_api_.Reset();
  event_generator_.Reset();
  transform_.Reset();
  d3d_manager_attached_ = false;
  input_stream_id_ = 0;
  output_stream_id_ = 0;
  output_provides_samples_ = true;
  output_buffer_size_ = 0;
  input_credits_ = 0;
  input_stride_ = 0;
  got_first_input_credit_ = false;
  // dxgi_manager_/d3d_device_/cached_activate_ are deliberately kept:
  // they're keyed by adapter LUID and reusable across candidates and
  // across rebuilds on the same adapter.
}

int32_t MediaFoundationH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::Release() {
  ResetTransformState();
  // The D3D device manager and the cached MFT activation deliberately
  // SURVIVE Release(): libwebrtc re-inits the encoder through
  // InitEncode→Release on every reconfigure (the dominant rebuild path),
  // and recreating the device + re-enumerating MFTs there is what made
  // every resolution step a long freeze. Both die in the destructor.
  initialized_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!initialized_ || !transform_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!encoded_image_callback_) {
    RTC_LOG(LS_WARNING)
        << "InitEncode() has been called, but a callback function "
           "has not been set with RegisterEncodeCompleteCallback()";
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (frame_types && !frame_types->empty() &&
      (*frame_types)[0] == VideoFrameType::kEmptyFrame) {
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }
  if (!sending_) {
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  scoped_refptr<I420BufferInterface> frame_buffer =
      input_frame.video_frame_buffer()->ToI420();
  if (!frame_buffer) {
    RTC_LOG(LS_ERROR) << "Failed to convert input image to I420.";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  if (frame_buffer->width() != codec_.width ||
      frame_buffer->height() != codec_.height) {
    // Resolution changed under us (libwebrtc also drives this via
    // InitEncode in most paths; this covers the rest). Prefer a dynamic
    // format change on the live MFT — a full pipeline rebuild is a visible
    // freeze and is exactly what made BWE-driven resolution steps hitch.
    RTC_LOG(LS_INFO) << "MF encoder resolution change "
                     << codec_.width << "x" << codec_.height << " -> "
                     << frame_buffer->width() << "x" << frame_buffer->height();
    codec_.width = static_cast<uint16_t>(frame_buffer->width());
    codec_.height = static_cast<uint16_t>(frame_buffer->height());
    if (!TryInPlaceFormatChange()) {
      int32_t ret = CreateAndConfigureTransform();
      if (ret != WEBRTC_VIDEO_CODEC_OK) {
        Release();
        return ret;
      }
    }
    pending_rate_reinit_ = false;
    force_key_frame_ = true;
  }

  if (pending_rate_reinit_) {
    // A vendor that refuses dynamic bitrate updates drifted >=30% from the
    // target (see SetRates). The in-place path re-sets MF_MT_AVG_BITRATE
    // and the CodecAPI knobs without tearing the session down.
    pending_rate_reinit_ = false;
    if (TryInPlaceFormatChange() ||
        CreateAndConfigureTransform() == WEBRTC_VIDEO_CODEC_OK) {
      force_key_frame_ = true;
    } else {
      Release();
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
  }

  bool send_key_frame =
      force_key_frame_ ||
      (frame_types && !frame_types->empty() &&
       (*frame_types)[0] == VideoFrameType::kVideoFrameKey);
  if (send_key_frame && codec_api_) {
    SetCodecApiU32(codec_api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
  }
  force_key_frame_ = false;

  // I420 -> NV12 into an MF sample (CPU path; ~1-2ms at 1440p via libyuv).
  // Rows use the stride the MFT negotiated (>= width on padding vendors).
  const int width = frame_buffer->width();
  const int height = frame_buffer->height();
  const int stride =
      static_cast<int>(input_stride_ >= static_cast<uint32_t>(width)
                           ? input_stride_
                           : static_cast<uint32_t>(width));
  const DWORD nv12_size = static_cast<DWORD>(stride) * height * 3 / 2;

  ComPtr<IMFMediaBuffer> media_buffer;
  HRESULT hr =
      MFCreateAlignedMemoryBuffer(nv12_size, MF_64_BYTE_ALIGNMENT,
                                  &media_buffer);
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  BYTE* dst = nullptr;
  DWORD max_len = 0;
  hr = media_buffer->Lock(&dst, &max_len, nullptr);
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  uint8_t* dst_y = dst;
  uint8_t* dst_uv = dst + static_cast<size_t>(stride) * height;
  libyuv::I420ToNV12(frame_buffer->DataY(), frame_buffer->StrideY(),
                     frame_buffer->DataU(), frame_buffer->StrideU(),
                     frame_buffer->DataV(), frame_buffer->StrideV(), dst_y,
                     stride, dst_uv, stride, width, height);
  media_buffer->Unlock();
  media_buffer->SetCurrentLength(nv12_size);

  ComPtr<IMFSample> sample;
  hr = MFCreateSample(&sample);
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  sample->AddBuffer(media_buffer.Get());

  const int64_t sample_time_100ns = input_frame.timestamp_us() * 10;
  sample->SetSampleTime(sample_time_100ns);
  sample->SetSampleDuration(10'000'000ll /
                            std::max<uint32_t>(codec_.maxFramerate, 1));

  pending_frames_.push_back(PendingFrameInfo{
      sample_time_100ns,
      input_frame.rtp_timestamp(),
      input_frame.ntp_time_ms(),
      input_frame.render_time_ms(),
      input_frame.rotation(),
  });
  // Bound the queue: a wedged MFT must not grow this unboundedly.
  while (pending_frames_.size() > 8) {
    pending_frames_.pop_front();
  }

  pending_input_ = sample;
  return PumpEvents(/*wait_for_input_credit=*/true);
}

int32_t MediaFoundationH264EncoderImpl::PumpEvents(
    bool wait_for_input_credit) {
  const int64_t deadline_ms =
      TimeMillis() + (got_first_input_credit_
                          ? kPumpDeadlineMs
                          : (any_session_warmed_ ? kRebuildPumpDeadlineMs
                                                 : kFirstPumpDeadlineMs));

  while (true) {
    if (pending_input_ && input_credits_ > 0) {
      int32_t ret = DeliverPendingInput();
      if (ret != WEBRTC_VIDEO_CODEC_OK) {
        return ret;
      }
    }

    ComPtr<IMFMediaEvent> event;
    HRESULT hr = event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) {
      if (!pending_input_ || !wait_for_input_credit) {
        return WEBRTC_VIDEO_CODEC_OK;
      }
      if (TimeMillis() > deadline_ms) {
        RTC_LOG(LS_ERROR)
            << "MF encoder stalled waiting for input credit; resetting.";
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
      ::Sleep(1);
      continue;
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "IMFMediaEventGenerator::GetEvent failed (hr="
                        << HexHr(hr) << ")";
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }

    MediaEventType type = MEUnknown;
    event->GetType(&type);
    switch (type) {
      case METransformNeedInput:
        got_first_input_credit_ = true;
        any_session_warmed_ = true;
        input_credits_++;
        break;
      case METransformHaveOutput: {
        int32_t ret = DrainOneOutput();
        if (ret != WEBRTC_VIDEO_CODEC_OK) {
          return ret;
        }
        break;
      }
      case METransformDrainComplete:
      case METransformMarker:
        break;
      case MEError: {
        HRESULT status = S_OK;
        event->GetStatus(&status);
        RTC_LOG(LS_ERROR) << "MF encoder MEError (hr=" << HexHr(status)
                          << ")";
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
      default:
        break;
    }
  }
}

int32_t MediaFoundationH264EncoderImpl::DeliverPendingInput() {
  HRESULT hr = transform_->ProcessInput(input_stream_id_,
                                        pending_input_.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT ProcessInput failed (hr=" << HexHr(hr) << ")";
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }
  input_credits_--;
  pending_input_.Reset();
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::DrainOneOutput() {
  MFT_OUTPUT_DATA_BUFFER output = {};
  output.dwStreamID = output_stream_id_;

  ComPtr<IMFSample> our_sample;
  if (!output_provides_samples_) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(output_buffer_size_, &buffer)) ||
        FAILED(MFCreateSample(&our_sample))) {
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    }
    our_sample->AddBuffer(buffer.Get());
    output.pSample = our_sample.Get();
  }

  DWORD status = 0;
  HRESULT hr = transform_->ProcessOutput(0, 1, &output, &status);

  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    // Renegotiate the output type and retry on the next HaveOutput event.
    ComPtr<IMFMediaType> new_type;
    if (SUCCEEDED(transform_->GetOutputAvailableType(output_stream_id_, 0,
                                                     &new_type))) {
      transform_->SetOutputType(output_stream_id_, new_type.Get(), 0);
    }
    if (output.pEvents) {
      output.pEvents->Release();
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }
  if (hr == E_UNEXPECTED || hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    // A HaveOutput event with no actual pending output — a stale or
    // contract-violating event (some vendors emit them around flushes).
    // Benign: skip this pump iteration instead of dumping the session to
    // the software fallback (Chromium does the same).
    if (output.pEvents) {
      output.pEvents->Release();
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "MFT ProcessOutput failed (hr=" << HexHr(hr) << ")";
    if (output.pEvents) {
      output.pEvents->Release();
    }
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  ComPtr<IMFSample> out_sample;
  if (output.pSample) {
    out_sample.Attach(output.pSample);  // owns the provided/created sample
  }
  if (output.pEvents) {
    output.pEvents->Release();
  }
  if (!out_sample) {
    return WEBRTC_VIDEO_CODEC_OK;
  }
  return ProcessOutputSample(std::move(out_sample));
}

int32_t MediaFoundationH264EncoderImpl::ProcessOutputSample(
    ComPtr<IMFSample> sample) {
  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
  if (FAILED(hr)) {
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  BYTE* data = nullptr;
  DWORD len = 0;
  hr = buffer->Lock(&data, nullptr, &len);
  if (FAILED(hr) || len == 0) {
    return FAILED(hr) ? WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE
                      : WEBRTC_VIDEO_CODEC_OK;
  }

  // Match the output back to its source frame. With B frames disabled the
  // MFT emits in presentation order, so the front of the queue is ours;
  // prefer an exact sample-time match when available.
  PendingFrameInfo info{};
  bool have_info = false;
  int64_t sample_time = 0;
  if (SUCCEEDED(sample->GetSampleTime(&sample_time))) {
    for (auto it = pending_frames_.begin(); it != pending_frames_.end();
         ++it) {
      if (it->sample_time_100ns == sample_time) {
        info = *it;
        pending_frames_.erase(pending_frames_.begin(), it + 1);
        have_info = true;
        break;
      }
    }
  }
  if (!have_info && !pending_frames_.empty()) {
    info = pending_frames_.front();
    pending_frames_.pop_front();
    have_info = true;
  }

  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  if (have_info) {
    encoded_image_.SetRtpTimestamp(info.rtp_timestamp);
    encoded_image_.ntp_time_ms_ = info.ntp_time_ms;
    encoded_image_.capture_time_ms_ = info.render_time_ms;
    encoded_image_.rotation_ = info.rotation;
  }
  encoded_image_.SetSimulcastIndex(0);
  encoded_image_.content_type_ = VideoContentType::SCREENSHARE;
  encoded_image_.timing_.flags = VideoSendTiming::kInvalid;
  encoded_image_._frameType = VideoFrameType::kVideoFrameDelta;

  std::vector<H264::NaluIndex> nalu_indices =
      H264::FindNaluIndices(MakeArrayView(data, len));
  for (const auto& index : nalu_indices) {
    if (H264::ParseNaluType(data[index.payload_start_offset]) == H264::kIdr) {
      encoded_image_._frameType = VideoFrameType::kVideoFrameKey;
      break;
    }
  }

  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(data, len));
  encoded_image_.set_size(len);
  buffer->Unlock();

  h264_bitstream_parser_.ParseBitstream(encoded_image_);
  encoded_image_.qp_ = h264_bitstream_parser_.GetLastSliceQp().value_or(-1);

  CodecSpecificInfo codec_info;
  codec_info.codecType = kVideoCodecH264;
  codec_info.codecSpecific.H264.packetization_mode =
      H264PacketizationMode::NonInterleaved;

  const auto result =
      encoded_image_callback_->OnEncodedImage(encoded_image_, &codec_info);
  if (result.error != EncodedImageCallback::Result::OK) {
    RTC_LOG(LS_ERROR) << "OnEncodedImage failed: " << result.error;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

bool MediaFoundationH264EncoderImpl::ApplyBitrate(uint32_t bitrate_bps) {
  if (!codec_api_) {
    return false;
  }
  // IsModifiable returns S_FALSE — which passes SUCCEEDED()! — when the
  // vendor refuses mid-session changes (seen on Intel MFTs). Treating that
  // as success used to latch configured_bitrate_bps_ without applying
  // anything, so the encoder ran at a stale rate forever, silently.
  HRESULT hr = codec_api_->IsModifiable(&CODECAPI_AVEncCommonMeanBitRate);
  if (hr != S_OK) {
    if (!bitrate_unmodifiable_logged_) {
      bitrate_unmodifiable_logged_ = true;
      RTC_LOG(LS_WARNING) << "MFT \"" << mft_friendly_name_
                          << "\": mid-session bitrate changes not supported "
                             "(IsModifiable hr="
                          << HexHr(hr)
                          << "); large rate changes will re-init the session";
    }
    return false;
  }
  if (FAILED(SetCodecApiU32(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                            bitrate_bps))) {
    return false;
  }
  // Keep the HRD window proportional to the new target (best effort; some
  // vendors lock it once streaming).
  SetCodecApiU32(codec_api_.Get(), CODECAPI_AVEncCommonBufferSize,
                 HrdBufferBitsFor(bitrate_bps));
  return true;
}

void MediaFoundationH264EncoderImpl::SetRates(
    const RateControlParameters& parameters) {
  if (!initialized_) {
    RTC_LOG(LS_WARNING) << "SetRates() while uninitialized.";
    return;
  }
  if (parameters.framerate_fps < 1.0) {
    return;
  }
  if (parameters.bitrate.get_sum_bps() == 0) {
    sending_ = false;
    return;
  }
  if (!sending_) {
    // Resuming after a pause: refresh with a keyframe.
    force_key_frame_ = true;
  }
  sending_ = true;

  codec_.maxFramerate = static_cast<uint32_t>(parameters.framerate_fps);
  target_bitrate_bps_ = parameters.bitrate.GetSpatialLayerSum(0);

  const int64_t now_ms = TimeMillis();
  if (target_bitrate_bps_ != configured_bitrate_bps_ &&
      now_ms - last_bitrate_update_ms_ >= kBitrateUpdateIntervalMs) {
    if (ApplyBitrate(target_bitrate_bps_)) {
      configured_bitrate_bps_ = target_bitrate_bps_;
    } else {
      // Vendor refused the dynamic change. Small drift is tolerable; past
      // ~30% the stream is either overshooting a congested link or stuck
      // soft on a recovered one, so re-init the session at the new rate.
      // Rate-limited: a rebuild is itself a hitch.
      const uint64_t reference = std::max(configured_bitrate_bps_, 1u);
      const uint64_t delta =
          target_bitrate_bps_ > configured_bitrate_bps_
              ? target_bitrate_bps_ - configured_bitrate_bps_
              : configured_bitrate_bps_ - target_bitrate_bps_;
      if (delta * 100 / reference >= 30 &&
          now_ms - last_rate_reinit_ms_ >= 5000) {
        last_rate_reinit_ms_ = now_ms;
        RTC_LOG(LS_WARNING)
            << "MF encoder will re-init for bitrate change "
            << configured_bitrate_bps_ << " -> " << target_bitrate_bps_
            << " bps (vendor refuses dynamic updates)";
        // Handled on the next Encode(), which owns the rebuild/fallback
        // error paths; a failed rebuild here would strand a dead transform
        // behind initialized_=true.
        pending_rate_reinit_ = true;
      }
    }
    last_bitrate_update_ms_ = now_ms;
  }
}

VideoEncoder::EncoderInfo MediaFoundationH264EncoderImpl::GetEncoderInfo()
    const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name =
      mft_friendly_name_.empty()
          ? "MediaFoundationH264"
          : "MediaFoundationH264 (" + mft_friendly_name_ + ")";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kI420,
                                  VideoFrameBuffer::Type::kNV12};
  return info;
}

}  // namespace webrtc
