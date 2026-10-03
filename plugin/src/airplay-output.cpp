// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay-output.hpp"

#include "airplay/annexb.hpp"
#include "airplay/encoder_policy.hpp"

#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr const char *kOutputId = "airplay_displays_output";

struct OutputData {
	obs_output_t *output;
	// Task 15 review ruling 3: atomic so encoded_packet (encoder thread) and
	// stop() (UI thread) can race safely; stop() nulls it before releasing
	// the output.
	std::atomic<AirPlayOutput *> owner;
};

const char *output_get_name(void *)
{
	return "Overflow";
}

void *output_create(obs_data_t *settings, obs_output_t *output)
{
	// The owner pointer travels in the settings: obs_output_info has no user
	// data for create().
	auto *owner = reinterpret_cast<AirPlayOutput *>(static_cast<intptr_t>(obs_data_get_int(settings, "owner")));
	auto *d = new OutputData{output, owner};
	if (owner)
		owner->attach_output_data(d); // ruling 3: so stop() can null owner before releasing
	return d;
}

void output_destroy(void *data)
{
	delete static_cast<OutputData *>(data);
}

bool output_start(void *data)
{
	auto *d = static_cast<OutputData *>(data);
	if (!obs_output_can_begin_data_capture(d->output, 0))
		return false;
	if (!obs_output_initialize_encoders(d->output, 0))
		return false;
	return obs_output_begin_data_capture(d->output, 0);
}

void output_stop(void *data, uint64_t)
{
	obs_output_end_data_capture(static_cast<OutputData *>(data)->output);
}

void output_encoded_packet(void *data, struct encoder_packet *packet)
{
	auto *d = static_cast<OutputData *>(data);
	AirPlayOutput *owner = d->owner.load();
	if (!owner)
		return; // ruling 3: stop() already nulled this; the output is tearing down
	if (!packet) {
		// libobs passes NULL when the encoder stops (obs-encoder.c
		// full_stop, e.g. on an encoder error). Ruling 1: stop audio too,
		// so it does not keep flowing into the queue without video.
		owner->stop_accepting();
		return;
	}
	owner->on_video_packet(packet);
}

void apply_setting(obs_data_t *s, const std::pair<std::string, airplay::SettingValue> &kv)
{
	if (const auto *i = std::get_if<int64_t>(&kv.second))
		obs_data_set_int(s, kv.first.c_str(), *i);
	else if (const auto *b = std::get_if<bool>(&kv.second))
		obs_data_set_bool(s, kv.first.c_str(), *b);
	else if (const auto *str = std::get_if<std::string>(&kv.second))
		obs_data_set_string(s, kv.first.c_str(), str->c_str());
}

} // namespace

AirPlayOutput::AirPlayOutput(airplay::MediaQueue &queue) : queue_(queue)
{
	keyframe_timer_.setSingleShot(true);
	keyframe_timer_.setTimerType(Qt::PreciseTimer);
	QObject::connect(&keyframe_timer_, &QTimer::timeout, [this] { request_keyframe(); });
}

AirPlayOutput::~AirPlayOutput()
{
	stop();
}

void AirPlayOutput::register_output_type()
{
	static obs_output_info info = {};
	info.id = kOutputId;
	info.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED; // raw audio comes from audio_output_connect
	info.get_name = output_get_name;
	info.create = output_create;
	info.destroy = output_destroy;
	info.start = output_start;
	info.stop = output_stop;
	info.encoded_packet = output_encoded_packet;
	info.encoded_video_codecs = "h264";
	obs_register_output(&info);
}

std::vector<std::string> AirPlayOutput::available_encoders()
{
	std::vector<std::string> ids;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
		if (!id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		const char *codec = obs_get_encoder_codec(id);
		if (!codec || std::strcmp(codec, "h264") != 0)
			continue;
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		ids.emplace_back(id);
	}
	return ids;
}

int AirPlayOutput::current_fps()
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || ovi.fps_den == 0)
		return 30;
	const long fps = std::lround(static_cast<double>(ovi.fps_num) / static_cast<double>(ovi.fps_den));
	return static_cast<int>(std::clamp(fps, 1L, 120L));
}

bool AirPlayOutput::active() const
{
	return output_ && obs_output_active(output_);
}

bool AirPlayOutput::request_keyframe()
{
	if (!encoder_ || !active() || encoder_id_.rfind("obs_nvenc_", 0) != 0)
		return false;
	const uint64_t now = os_gettime_ns();
	const airplay::KeyframeGate::Decision decision = keyframe_gate_.request(now);
	if (!decision.send_now) {
		// Several requests in one window share this one timer. Round up, so
		// it does not fire before the window ends (if it does, the gate
		// defers again and this re-arms it).
		if (!keyframe_timer_.isActive()) {
			const uint64_t wait_ns = decision.defer_until_ns > now ? decision.defer_until_ns - now : 0;
			keyframe_timer_.start(static_cast<int>((wait_ns + 999999ull) / 1000000ull) + 1);
		}
		return false;
	}
	obs_data_t *settings = obs_encoder_get_settings(encoder_);
	obs_encoder_update(encoder_, settings); // deferred to the encoder thread by libobs
	obs_data_release(settings);
	return true;
}

bool AirPlayOutput::start(const OutputConfig &config, std::string *error)
{
	if (output_) {
		if (obs_output_active(output_))
			return true;
		stop(); // it stopped on its own (encoder error); start fresh
	}

	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi)) {
		*error = "OBS video is not initialized";
		return false;
	}
	const airplay::PresetSpec spec = airplay::preset_spec(config.preset);
	const airplay::ScaledSize size =
		airplay::fit_within(ovi.output_width, ovi.output_height, spec.max_width, spec.max_height);
	const airplay::EncoderPlan plan = airplay::plan_encoders(available_encoders(), config.encoder_override);
	if (plan.override_ignored)
		obs_log(LOG_WARNING, "encoder override '%s' is not available; using the automatic order",
			config.encoder_override.c_str());

	obs_data_t *output_settings = obs_data_create();
	obs_data_set_int(output_settings, "owner", static_cast<long long>(reinterpret_cast<intptr_t>(this)));
	output_ = obs_output_create(kOutputId, "Overflow", output_settings, nullptr);
	obs_data_release(output_settings);
	if (!output_) {
		*error = "could not create the AirPlay output";
		return false;
	}

	mix_idx_ = static_cast<size_t>(std::clamp(config.audio_track, 1, 6) - 1);
	std::string last_error = "no H.264 encoder is available";
	for (const std::string &id : plan.candidates) {
		obs_data_t *es = obs_data_create();
		for (const auto &kv : airplay::encoder_settings(id, spec.bitrate_kbps))
			apply_setting(es, kv);
		obs_encoder_t *enc = obs_video_encoder_create(id.c_str(), "AirPlay H.264", es, nullptr);
		obs_data_release(es);
		if (!enc) {
			last_error = "could not create encoder " + id;
			continue;
		}
		obs_encoder_set_video(enc, obs_get_video());
		if (size.scaled) {
			obs_encoder_set_scaled_size(enc, size.width, size.height);
			// GPU scaling keeps texture encoders on the texture path
			// (obs-nvenc falls back to its non-texture path for CPU scaling).
			obs_encoder_set_gpu_scale_type(enc, OBS_SCALE_BICUBIC);
		}
		obs_output_set_video_encoder(output_, enc);
		encoder_ = enc;
		encoder_id_ = id;
		headers_.clear();
		headers_checked_ = false;
		accepting_ = true;
		current_window_age_ns_ = 0;
		last_window_age_ns_ = 0;
		window_started_ns_ = 0;
		audio_tap_active_ = false;
		if (obs_output_start(output_)) {
			audio_convert_info conv = {};
			conv.samples_per_sec = 44100;
			conv.format = AUDIO_FORMAT_16BIT; // interleaved S16
			conv.speakers = SPEAKERS_STEREO;
			conv.allow_clipping = true;
			// Ruling 2: audio_output_connect (unlike obs_add_raw_audio_callback)
			// returns a bool, so a tap that could not be wired up is reported
			// instead of silently dropped.
			if (audio_output_connect(obs_get_audio(), mix_idx_, &conv, audio_trampoline, this)) {
				audio_connected_ = true;
			} else {
				obs_log(LOG_WARNING, "audio tap unavailable; displays get video only");
			}
			obs_log(LOG_INFO, "AirPlay output started: encoder %s, %ux%u, %d kbps, audio track %d", id.c_str(),
				size.width, size.height, spec.bitrate_kbps, static_cast<int>(mix_idx_ + 1));
			return true;
		}
		accepting_ = false;
		const char *why = obs_output_get_last_error(output_);
		last_error = id + ": " + (why && *why ? why : "the output did not start");
		obs_log(LOG_WARNING, "AirPlay output: %s; trying the next encoder", last_error.c_str());
		obs_output_set_video_encoder(output_, nullptr);
		obs_encoder_release(enc);
		encoder_ = nullptr;
		encoder_id_.clear();
	}
	output_data_ = nullptr;
	obs_output_release(output_);
	output_ = nullptr;
	*error = last_error;
	return false;
}

void AirPlayOutput::stop()
{
	accepting_ = false;
	keyframe_timer_.stop();
	if (output_data_) {
		// Ruling 3: null the owner before releasing the output, closing the
		// window where encoded_packet could still dereference this object.
		static_cast<OutputData *>(output_data_)->owner = nullptr;
		output_data_ = nullptr;
	}
	if (audio_connected_) {
		audio_output_disconnect(obs_get_audio(), mix_idx_, audio_trampoline, this);
		audio_connected_ = false;
	}
	if (output_) {
		obs_output_stop(output_);
		obs_output_release(output_);
		output_ = nullptr;
	}
	if (encoder_) {
		obs_encoder_release(encoder_);
		encoder_ = nullptr;
	}
	encoder_id_.clear();
	audio_tap_active_ = false;
}

void AirPlayOutput::on_video_packet(struct encoder_packet *packet)
{
	if (!accepting_ || packet->type != OBS_ENCODER_VIDEO || !packet->data || packet->size == 0)
		return;
	if (!headers_checked_) {
		uint8_t *extra = nullptr;
		size_t extra_size = 0;
		if (packet->encoder && obs_encoder_get_extra_data(packet->encoder, &extra, &extra_size))
			headers_ = airplay::extradata_to_annexb(extra, extra_size);
		headers_checked_ = true;
		if (headers_.empty())
			obs_log(LOG_WARNING, "AirPlay output: the encoder has no SPS/PPS extra data; relying on in-band headers");
	}
	// sys_dts_usec is in the os_gettime_ns() clock (obs-encoder.c
	// send_off_encoder_packet); with no B-frames, pts == dts.
	const uint64_t capture_ns = packet->sys_dts_usec > 0 ? static_cast<uint64_t>(packet->sys_dts_usec) * 1000u
							     : os_gettime_ns();
	queue_.push_video(airplay::build_access_unit(packet->data, packet->size, packet->keyframe, headers_), capture_ns,
			  packet->keyframe);
}

namespace {
// Ruling R7: how long a window accumulates its max audio age before the
// value is published and the window resets.
constexpr uint64_t kAudioAgeWindowNs = 2000000000ull;
} // namespace

void AirPlayOutput::on_audio(struct audio_data *data)
{
	if (!accepting_ || !data || !data->data[0] || data->frames == 0)
		return;
	const size_t bytes = static_cast<size_t>(data->frames) * 4u; // S16 stereo, interleaved
	std::vector<uint8_t> pcm(data->data[0], data->data[0] + bytes);
	const uint64_t now_ns = os_gettime_ns();
	const uint64_t timestamp_ns = data->timestamp ? data->timestamp : now_ns;
	queue_.push_audio(std::move(pcm), timestamp_ns);

	// Age tracking (audio thread; never blocks). Clamp at 0: a timestamp at or
	// after "now" (clock skew, or the first sample of a session) is not late.
	audio_tap_active_ = true;
	const uint64_t age_ns = now_ns > timestamp_ns ? now_ns - timestamp_ns : 0;

	uint64_t window_start = window_started_ns_.load();
	if (window_start == 0) {
		// First sample of a fresh window: seed it rather than racing a
		// compare_exchange against 0 from multiple threads (there is only
		// one audio thread calling in, but this keeps the intent explicit).
		window_started_ns_.store(now_ns);
		window_start = now_ns;
	}

	uint64_t current = current_window_age_ns_.load();
	while (age_ns > current && !current_window_age_ns_.compare_exchange_weak(current, age_ns)) {
	}

	if (now_ns - window_start >= kAudioAgeWindowNs) {
		last_window_age_ns_.store(current_window_age_ns_.exchange(0));
		window_started_ns_.store(now_ns);
	}
}

void AirPlayOutput::audio_trampoline(void *param, size_t, struct audio_data *data)
{
	static_cast<AirPlayOutput *>(param)->on_audio(data);
}
