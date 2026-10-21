#include "outputs.hpp"
#include "encode.hpp"
#include "settings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <string>
#include <vector>

namespace livedelay {

namespace {

struct DestOutput {
	std::string name;
	std::string ingest_key;
	obs_output_t *output = nullptr;
	obs_encoder_t *video = nullptr;
	std::vector<obs_encoder_t *> extra_video; // EB ladder (idx 1…)
	obs_encoder_t *vod_audio = nullptr;
	obs_service_t *service = nullptr;
	bool want = false;
};

std::vector<DestOutput> g_outs;
obs_encoder_t *g_audio = nullptr;
bool g_inited = false;

void EnableCustomServerVodTrack()
{
	// OBS only offers VOD on Twitch service unless this user.ini flag is set.
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return;
	if (!config_get_bool(cfg, "General", "EnableCustomServerVodTrack")) {
		config_set_bool(cfg, "General", "EnableCustomServerVodTrack", true);
		config_save_safe(cfg, "tmp", nullptr);
		blog(LOG_INFO, "[livedelay] enabled EnableCustomServerVodTrack for Twitch VOD on custom RTMP");
	}
}

// When a dest wants VOD audio, also flip AdvOut VodTrack on (Advanced mode only).
void EnsureAdvOutVodTrack()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (!(mode && std::string(mode) == "Advanced")) {
		blog(LOG_WARNING, "[livedelay] VOD audio needs OBS Output mode Advanced + Vod Track");
		return;
	}
	bool changed = false;
	if (!config_get_bool(cfg, "AdvOut", "VodTrackEnabled")) {
		config_set_bool(cfg, "AdvOut", "VodTrackEnabled", true);
		changed = true;
	}
	int live = int(config_get_int(cfg, "AdvOut", "TrackIndex"));
	int vod = int(config_get_int(cfg, "AdvOut", "VodTrackIndex"));
	if (vod < 1 || vod > 6)
		vod = 2;
	if (vod == live) {
		vod = live == 2 ? 1 : 2;
		config_set_int(cfg, "AdvOut", "VodTrackIndex", vod);
		changed = true;
	}
	if (changed) {
		config_save_safe(cfg, "tmp", nullptr);
		blog(LOG_INFO, "[livedelay] enabled AdvOut VodTrack (live=%d vod=%d)", live, vod);
	}
	EnableCustomServerVodTrack();
}

void ReleaseOne(DestOutput &o)
{
	if (o.output) {
		if (obs_output_active(o.output))
			obs_output_force_stop(o.output);
		obs_output_set_audio_encoder(o.output, nullptr, 1);
		obs_output_release(o.output);
		o.output = nullptr;
	}
	if (o.video) {
		obs_encoder_release(o.video);
		o.video = nullptr;
	}
	for (obs_encoder_t *e : o.extra_video) {
		if (e)
			obs_encoder_release(e);
	}
	o.extra_video.clear();
	if (o.vod_audio) {
		obs_encoder_release(o.vod_audio);
		o.vod_audio = nullptr;
	}
	if (o.service) {
		obs_service_release(o.service);
		o.service = nullptr;
	}
}

void ReleaseAll()
{
	for (DestOutput &o : g_outs)
		ReleaseOne(o);
	g_outs.clear();
	if (g_audio) {
		obs_encoder_release(g_audio);
		g_audio = nullptr;
	}
}

obs_data_t *VideoSettings(const EncodeSettings &e, int kbps_override = 0)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "rate_control", e.rate_control.toUtf8().constData());
	obs_data_set_int(s, "bitrate", kbps_override > 0 ? kbps_override : e.video_kbps);
	obs_data_set_int(s, "keyint_sec", e.keyint_sec);
	obs_data_set_string(s, "preset", "hq");
	obs_data_set_string(s, "preset2", "p5");
	obs_data_set_string(s, "tune", "hq");
	obs_data_set_string(s, "multipass", "qres");
	obs_data_set_string(s, "profile", "high");
	obs_data_set_bool(s, "psycho_aq", true);
	return s;
}

obs_data_t *AudioSettings(int kbps)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "bitrate", kbps);
	return s;
}

size_t LiveAudioMixer()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return 0;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (mode && std::string(mode) == "Advanced") {
		int track = int(config_get_int(cfg, "AdvOut", "TrackIndex"));
		if (track < 1 || track > 6)
			track = 1;
		return size_t(track - 1);
	}
	return 0;
}

bool VodMixer(size_t &out_mixer)
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return false;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (!(mode && std::string(mode) == "Advanced"))
		return false;
	if (!config_get_bool(cfg, "AdvOut", "VodTrackEnabled"))
		return false;
	int live = int(config_get_int(cfg, "AdvOut", "TrackIndex"));
	int vod = int(config_get_int(cfg, "AdvOut", "VodTrackIndex"));
	if (vod < 1 || vod > 6)
		vod = 2;
	if (live == vod)
		return false;
	out_mixer = size_t(vod - 1);
	return true;
}

bool EnsureAudio(int kbps)
{
	if (g_audio)
		return true;
	const QString id = ObsStreamAudioEncoderId();
	obs_data_t *as = AudioSettings(kbps > 0 ? kbps : 160);
	g_audio = obs_audio_encoder_create(id.toUtf8().constData(), "livedelay_audio", as, LiveAudioMixer(), nullptr);
	obs_data_release(as);
	if (!g_audio) {
		blog(LOG_ERROR, "[livedelay] could not create audio encoder %s", id.toUtf8().constData());
		return false;
	}
	obs_encoder_set_audio(g_audio, obs_get_audio());
	return true;
}

bool EnsureVodAudio(DestOutput &o, int kbps)
{
	if (o.vod_audio)
		return true;
	size_t mixer = 0;
	if (!VodMixer(mixer)) {
		blog(LOG_WARNING, "[livedelay] VOD audio requested but OBS AdvOut VodTrack is off or same as live track");
		return false;
	}
	EnableCustomServerVodTrack();
	const QString id = ObsStreamAudioEncoderId();
	obs_data_t *as = AudioSettings(kbps > 0 ? kbps : 160);
	const std::string name = "livedelay_vod_" + o.ingest_key;
	o.vod_audio = obs_audio_encoder_create(id.toUtf8().constData(), name.c_str(), as, mixer, nullptr);
	obs_data_release(as);
	if (!o.vod_audio)
		return false;
	obs_encoder_set_audio(o.vod_audio, obs_get_audio());
	return true;
}

bool BuildOne(DestOutput &o, const Destination &d, size_t index)
{
	ReleaseOne(o);
	o.name = d.name;
	o.ingest_key = d.ingest_key.empty() ? ("ld" + std::to_string(index)) : d.ingest_key;

	const EncodeSettings enc = ResolveEncode(d);
	if (!EnsureAudio(enc.audio_kbps))
		return false;

	obs_data_t *vs = VideoSettings(enc);
	const std::string vname = "livedelay_v_" + o.ingest_key;
	o.video = obs_video_encoder_create(enc.video_encoder_id.toUtf8().constData(), vname.c_str(), vs, nullptr);
	obs_data_release(vs);
	if (!o.video) {
		blog(LOG_ERROR, "[livedelay] video encoder %s failed for %s", enc.video_encoder_id.toUtf8().constData(),
		     o.name.c_str());
		return false;
	}
	obs_encoder_set_video(o.video, obs_get_video());
	if (enc.width > 0 && enc.height > 0)
		obs_encoder_set_scaled_size(o.video, enc.width, enc.height);

	const int port = Settings::Load(ConfigPath()).RtmpPort();
	const std::string server = "rtmp://127.0.0.1:" + std::to_string(port) + "/live";
	obs_data_t *svc = obs_data_create();
	obs_data_set_string(svc, "server", server.c_str());
	obs_data_set_string(svc, "key", o.ingest_key.c_str());
	const std::string sname = "livedelay_svc_" + o.ingest_key;
	o.service = obs_service_create("rtmp_custom", sname.c_str(), svc, nullptr);
	obs_data_release(svc);
	if (!o.service) {
		blog(LOG_ERROR, "[livedelay] service failed for %s", o.name.c_str());
		ReleaseOne(o);
		return false;
	}

	const std::string oname = "livedelay_out_" + o.ingest_key;
	o.output = obs_output_create("rtmp_output", oname.c_str(), nullptr, nullptr);
	if (!o.output) {
		blog(LOG_ERROR, "[livedelay] rtmp_output failed for %s", o.name.c_str());
		ReleaseOne(o);
		return false;
	}
	obs_output_set_service(o.output, o.service);
	obs_output_set_video_encoder(o.output, o.video);
	obs_output_set_audio_encoder(o.output, g_audio, 0);

	// NEVER attach obs_output_set_video_encoder2 for stock Twitch/YouTube RTMP.
	// Multi-video Enhanced RTMP without Twitch GoLive produces unplayable media
	// (Twitch player error #1000). Ladder stays off until GoLive ingest exists.
	if (d.enhanced_broadcasting) {
		blog(LOG_WARNING,
		     "[livedelay] Enhanced Broadcasting requested for %s but ignored — "
		     "multi-video RTMP breaks Twitch without GoLive (player #1000). "
		     "Publishing single video track only.",
		     o.name.c_str());
	}

	if (d.vod_audio) {
		EnsureAdvOutVodTrack();
		if (EnsureVodAudio(o, enc.audio_kbps))
			obs_output_set_audio_encoder(o.output, o.vod_audio, 1);
	}

	blog(LOG_INFO, "[livedelay] ready %s → %s key=%s (%s %d kb/s)%s%s", o.name.c_str(), server.c_str(),
	     o.ingest_key.c_str(), enc.video_encoder_id.toUtf8().constData(), enc.video_kbps,
	     d.vod_audio ? " +VOD" : "", d.enhanced_broadcasting ? " +EB(ignored)" : "");
	return true;
}

bool StartIndex(size_t i);

void Rebuild(bool restart_live)
{
	std::vector<bool> was_live;
	was_live.reserve(g_outs.size());
	for (DestOutput &o : g_outs) {
		const bool live = o.want || (o.output && obs_output_active(o.output));
		was_live.push_back(live);
		if (o.output && obs_output_active(o.output))
			obs_output_force_stop(o.output);
	}
	ReleaseAll();

	Settings s = Settings::Load(ConfigPath());
	g_outs.resize(s.dests.size());
	for (size_t i = 0; i < s.dests.size(); ++i) {
		if (s.dests[i].ingest_key.empty())
			s.dests[i].ingest_key = "ld" + std::to_string(i);
		BuildOne(g_outs[i], s.dests[i], i);
	}
	if (restart_live) {
		for (size_t i = 0; i < g_outs.size(); ++i) {
			if (i < was_live.size() && was_live[i] && s.dests[i].enabled)
				StartIndex(i);
		}
	}
}

bool StartIndex(size_t i)
{
	if (i >= g_outs.size())
		return false;
	DestOutput &o = g_outs[i];
	if (!o.output) {
		Settings s = Settings::Load(ConfigPath());
		if (i >= s.dests.size())
			return false;
		if (!BuildOne(o, s.dests[i], i))
			return false;
	}
	if (obs_output_active(o.output))
		return true;
	if (!obs_output_start(o.output)) {
		const char *err = obs_output_get_last_error(o.output);
		blog(LOG_WARNING, "[livedelay] could not start output for %s: %s", o.name.c_str(), err ? err : "?");
		return false;
	}
	o.want = true;
	blog(LOG_INFO, "[livedelay] started encode for %s", o.name.c_str());
	return true;
}

void StopIndex(size_t i)
{
	if (i >= g_outs.size())
		return;
	DestOutput &o = g_outs[i];
	o.want = false;
	if (o.output && obs_output_active(o.output))
		obs_output_stop(o.output);
}

void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		HardenObsForLiveDelay();
	else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING)
		OutputsOnStreamingStarting();
	else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPING || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED)
		OutputsOnStreamingStopping();
	else if (event == OBS_FRONTEND_EVENT_EXIT)
		OutputsShutdown();
}

} // namespace

void OutputsEnsureCustomServerVodTrack()
{
	EnableCustomServerVodTrack();
}

void OutputsInit()
{
	if (g_inited)
		return;
	g_inited = true;
	Rebuild(false);
	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
	// Module may load after FINISHED_LOADING (rare); harden immediately too.
	HardenObsForLiveDelay();
}

void OutputsShutdown()
{
	if (!g_inited)
		return;
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	ReleaseAll();
	g_inited = false;
}

void OutputsOnStreamingStarting()
{
	// Last chance before we encode to platforms: stock Multitrack must stay off.
	HardenObsForLiveDelay();
	if (g_outs.empty())
		Rebuild(false);
	Settings s = Settings::Load(ConfigPath());
	for (size_t i = 0; i < s.dests.size() && i < g_outs.size(); ++i) {
		// Never honor enhanced_broadcasting on the wire (see BuildOne).
		s.dests[i].enhanced_broadcasting = false;
		if (s.dests[i].autostart && s.dests[i].enabled)
			StartIndex(i);
	}
}

void OutputsOnStreamingStopping()
{
	for (size_t i = 0; i < g_outs.size(); ++i)
		StopIndex(i);
}

bool OutputStartDest(size_t index)
{
	if (g_outs.empty())
		Rebuild(false);
	return StartIndex(index);
}

void OutputStopDest(size_t index)
{
	StopIndex(index);
}

void OutputsReloadConfig()
{
	// Encode / platform list changed: rebuild and put back whatever was live.
	Rebuild(true);
}

} // namespace livedelay
