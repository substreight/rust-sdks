/*
 * Copyright 2025 LiveKit, Inc.
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

#include "livekit/peer_connection_factory.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/environment/deprecated_global_field_trials.h"
#include "api/environment/environment_factory.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/enable_media.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/audio/audio_device.h"
#include "api/audio_options.h"
#include "livekit/adm_proxy.h"
#include "livekit/audio_track.h"
#include "livekit/peer_connection.h"
#include "livekit/rtc_error.h"
#include "livekit/rtp_parameters.h"
#include "livekit/video_decoder_factory.h"
#include "livekit/video_encoder_factory.h"
#include "livekit/webrtc.h"
#include "rtc_base/thread.h"
#include "webrtc-sys/src/peer_connection.rs.h"
#include "webrtc-sys/src/peer_connection_factory.rs.h"

namespace livekit_ffi {

class PeerConnectionObserver;

namespace {

// Sanity check for "Trial1/Group1/Trial2/Group2/" strings: the legacy
// global registry used below only DCHECKs the format (release builds would
// silently misparse), and the input is a user-controlled env var.
bool LooksLikeValidFieldTrialsString(const std::string& s) {
  if (s.empty() || s.back() != '/') {
    return false;
  }
  size_t segments = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '/') {
      if (i == start) {
        return false;  // empty segment
      }
      ++segments;
      start = i + 1;
    }
  }
  return segments % 2 == 0;
}

// Field-trial injection point. CAIRN_WEBRTC_FIELD_TRIALS holds a standard
// libwebrtc trial string, e.g.
//   WebRTC-Bwe-ProbingConfiguration/alr_interval:2s,alr_scale:3/
//   WebRTC-Video-MinVideoBitrate/Enabled,h264_br:2500kbps/
// (note: some trials require the "Enabled" group prefix). Default empty —
// no behavior change unless the host opts in. The factory is a per-session
// singleton, so trials can change between shares without an app restart.
//
// Uses the deprecated GLOBAL registry rather than webrtc::FieldTrials: the
// prebuilt LiveKit webrtc.lib ships DeprecatedGlobalFieldTrials but does
// not compile api/field_trials.cc, so FieldTrials::Create does not link.
webrtc::Environment CreateEnvironmentWithFieldTrials() {
  webrtc::EnvironmentFactory factory;
  std::string trials;
#ifdef _WIN32
  // GetEnvironmentVariableW, not CRT getenv: the host (Rust std::env) sets
  // variables via SetEnvironmentVariableW, which does not refresh the CRT's
  // cached _environ, so getenv() can miss values set after process start.
  wchar_t buf[4096];
  DWORD len =
      ::GetEnvironmentVariableW(L"CAIRN_WEBRTC_FIELD_TRIALS", buf, 4096);
  if (len > 0 && len < 4096) {
    int utf8_len = ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                                         nullptr, 0, nullptr, nullptr);
    trials.resize(utf8_len);
    ::WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                          trials.data(), utf8_len, nullptr, nullptr);
  }
#else
  const char* raw = std::getenv("CAIRN_WEBRTC_FIELD_TRIALS");
  if (raw) {
    trials = raw;
  }
#endif
  if (!trials.empty()) {
    if (LooksLikeValidFieldTrialsString(trials)) {
      RTC_LOG(LS_INFO) << "WebRTC field trials active: " << trials;
      // The global registry stores the RAW POINTER — the string must outlive
      // the process. Deliberately leaked (at most once per factory creation,
      // i.e. per streaming session with a changed value).
      char* leaked = new char[trials.size() + 1];
      memcpy(leaked, trials.c_str(), trials.size() + 1);
      webrtc::DeprecatedGlobalFieldTrials::Set(leaked);
      factory.Set(std::make_unique<webrtc::DeprecatedGlobalFieldTrials>());
    } else {
      RTC_LOG(LS_ERROR) << "CAIRN_WEBRTC_FIELD_TRIALS is malformed (want "
                           "\"Trial/Group/.../\") and was ignored: "
                        << trials;
    }
  }
  return factory.Create();
}

}  // namespace

PeerConnectionFactory::PeerConnectionFactory(
    std::shared_ptr<RtcRuntime> rtc_runtime)
    : rtc_runtime_(rtc_runtime),
    env_(CreateEnvironmentWithFieldTrials()) {
  webrtc::PeerConnectionFactoryDependencies dependencies;
  // Hand the factory OUR Environment (which carries the field trials).
  // Without this, CreateModularPeerConnectionFactory builds its own default
  // Environment and the trials silently never reach BWE / the probe
  // controller / the quality scaler — env_ alone only feeds the AdmProxy.
  dependencies.env = env_;
  dependencies.network_thread = rtc_runtime_->network_thread();
  dependencies.worker_thread = rtc_runtime_->worker_thread();
  dependencies.signaling_thread = rtc_runtime_->signaling_thread();
  dependencies.socket_factory = rtc_runtime_->network_thread()->socketserver();
  dependencies.event_log_factory = std::make_unique<webrtc::RtcEventLogFactory>();

  // Create AdmProxy - it creates and initializes Platform ADM internally
  adm_proxy_ = rtc_runtime_->worker_thread()->BlockingCall([&] {
    return webrtc::make_ref_counted<livekit_ffi::AdmProxy>(
        env_, rtc_runtime_->worker_thread());
  });
  audio_device_ = std::make_shared<AudioDeviceController>(adm_proxy_);

  dependencies.adm = adm_proxy_;

  dependencies.video_encoder_factory =
      std::move(std::make_unique<livekit_ffi::VideoEncoderFactory>());
  dependencies.video_decoder_factory =
      std::move(std::make_unique<livekit_ffi::VideoDecoderFactory>());
  dependencies.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
  dependencies.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
  dependencies.audio_processing_builder = std::make_unique<webrtc::BuiltinAudioProcessingBuilder>();

  webrtc::EnableMedia(dependencies);
  peer_factory_ =
      webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));

  if (peer_factory_.get() == nullptr) {
    RTC_LOG_ERR(LS_ERROR) << "Failed to create PeerConnectionFactory";
    return;
  }
}

PeerConnectionFactory::~PeerConnectionFactory() {
  RTC_LOG(LS_VERBOSE) << "PeerConnectionFactory::~PeerConnectionFactory()";

  peer_factory_ = nullptr;
  audio_device_ = nullptr;
  rtc_runtime_->worker_thread()->BlockingCall(
      [this] { adm_proxy_ = nullptr; });
}

std::shared_ptr<PeerConnection> PeerConnectionFactory::create_peer_connection(
    RtcConfiguration config,
    rust::Box<PeerConnectionObserverWrapper> observer) const {
  std::shared_ptr<PeerConnection> pc = std::make_shared<PeerConnection>(
      rtc_runtime_, peer_factory_, std::move(observer));

  if (!pc->Initialize(to_native_rtc_configuration(config))) {
    throw std::runtime_error(serialize_error(to_error(webrtc::RTCError(
        webrtc::RTCErrorType::INTERNAL_ERROR, "failed to initialize pc"))));
  }

  return pc;
}

std::shared_ptr<VideoTrack> PeerConnectionFactory::create_video_track(
    rust::String label,
    std::shared_ptr<VideoTrackSource> source) const {
  return std::static_pointer_cast<VideoTrack>(
      rtc_runtime_->get_or_create_media_stream_track(
          peer_factory_->CreateVideoTrack(source->get(), label.c_str())));
}

std::shared_ptr<AudioTrack> PeerConnectionFactory::create_audio_track(
    rust::String label,
    std::shared_ptr<AudioTrackSource> source) const {
  return std::static_pointer_cast<AudioTrack>(
      rtc_runtime_->get_or_create_media_stream_track(
          peer_factory_->CreateAudioTrack(label.c_str(), source->get().get())));
}

std::shared_ptr<AudioTrack> PeerConnectionFactory::create_device_audio_track(
    rust::String label) const {
  // Create an audio source that uses the ADM for capture
  webrtc::AudioOptions audio_options;
  audio_options.echo_cancellation = true;
  audio_options.auto_gain_control = true;
  audio_options.noise_suppression = true;

  webrtc::scoped_refptr<webrtc::AudioSourceInterface> audio_source =
      peer_factory_->CreateAudioSource(audio_options);

  if (!audio_source) {
    RTC_LOG(LS_ERROR) << "Failed to create device audio source";
    return nullptr;
  }

  return std::static_pointer_cast<AudioTrack>(
      rtc_runtime_->get_or_create_media_stream_track(
          peer_factory_->CreateAudioTrack(label.c_str(), audio_source.get())));
}

RtpCapabilities PeerConnectionFactory::rtp_sender_capabilities(
    MediaType type) const {
  return to_rust_rtp_capabilities(peer_factory_->GetRtpSenderCapabilities(
      static_cast<webrtc::MediaType>(type)));
}

RtpCapabilities PeerConnectionFactory::rtp_receiver_capabilities(
    MediaType type) const {
  return to_rust_rtp_capabilities(peer_factory_->GetRtpReceiverCapabilities(
      static_cast<webrtc::MediaType>(type)));
}

std::shared_ptr<AudioDeviceController> PeerConnectionFactory::audio_device() const {
  return audio_device_;
}

std::shared_ptr<PeerConnectionFactory> create_peer_connection_factory() {
  return std::make_shared<PeerConnectionFactory>(RtcRuntime::create());
}

}  // namespace livekit_ffi
