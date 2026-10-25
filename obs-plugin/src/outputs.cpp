#include "outputs.hpp"
#include "client.hpp"
#include "encode.hpp"
#include "golive.hpp"
#include "settings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstring>
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
	obs_encoder_group_t *group = nullptr;
	obs_encoder_t *audio = nullptr; // pooled, not owned
	obs_service_t *service = nullptr;
	bool want = false;
	bool tried_x264 = false;
	bool golive = false;
	std::string egress_url;
	std::string egress_key;
};

struct PooledAudio {
	size_t mixer = 0;
	int kbps = 0;
	obs_encoder_t *enc = nullptr;
};

std::vector<DestOutput> g_outs;
std::vector<PooledAudio> g_audio;
bool g_inited = false;
// Platform encoders must not start inside STREAMING_STARTING: OBS has not
// connected to this proxy yet, and sleeping there blocks that connection.
QTimer *g_arm = nullptr;
bool g_arm_pending = false;
int g_arm_waited_ms = 0;
int g_arm_noted = 0;

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

void ReleaseOne(DestOutput &o)
{
	if (o.output) {
		if (obs_output_active(o.output))
			obs_output_force_stop(o.output);
		obs_output_set_audio_encoder(o.output, nullptr, 0);
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
	if (o.group) {
		obs_encoder_group_destroy(o.group);
		o.group = nullptr;
	}
	o.golive = false;
	o.egress_url.clear();
	o.egress_key.clear();
	o.audio = nullptr;
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
	for (PooledAudio &p : g_audio) {
		if (p.enc)
			obs_encoder_release(p.enc);
	}
	g_audio.clear();
}

obs_data_t *VideoSettings(const EncodeSettings &e, int kbps_override = 0)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "rate_control", e.rate_control.toUtf8().constData());
	obs_data_set_int(s, "bitrate", kbps_override > 0 ? kbps_override : e.video_kbps);
	obs_data_set_int(s, "keyint_sec", e.keyint_sec);
	obs_data_set_string(s, "profile", "high");
	obs_data_set_int(s, "bf", 2);

	// Encoder-specific knobs. Applying NVENC "hq"/"p5" to obs_x264 made
	// Invalid preset/tune in the log and produced a Twitch-unplayable
	// bitstream (player error #1000) while kbps still looked fine.
	const QString id = e.video_encoder_id.toLower();
	if (id.contains(QStringLiteral("nvenc"))) {
		obs_data_set_string(s, "preset", "p5");
		obs_data_set_string(s, "preset2", "p5");
		obs_data_set_string(s, "tune", "hq");
		obs_data_set_string(s, "multipass", "disabled");
		obs_data_set_bool(s, "psycho_aq", true);
	} else if (id.contains(QStringLiteral("amf")) || id.contains(QStringLiteral("amd"))) {
		obs_data_set_string(s, "preset", "quality");
	} else if (id.contains(QStringLiteral("qsv"))) {
		obs_data_set_string(s, "target_usage", "balanced");
	} else {
		// obs_x264 (and unknown software encoders)
		obs_data_set_string(s, "preset", "veryfast");
	}
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

bool DestIsTwitch(const Destination &d)
{
	const QString svc = QString::fromStdString(d.service);
	const QString url = QString::fromStdString(d.url);
	return svc.contains(QStringLiteral("twitch"), Qt::CaseInsensitive) ||
	       url.contains(QStringLiteral("twitch"), Qt::CaseInsensitive) ||
	       url.contains(QStringLiteral("live-video.net"), Qt::CaseInsensitive);
}

size_t ResolveLiveMixer(const Destination &d)
{
	if (d.audio_track >= 1 && d.audio_track <= 6)
		return size_t(d.audio_track - 1);
	return LiveAudioMixer();
}

// Twitch stores audio encoder index 1 as the VOD. It must be a different mixer than live.
int ResolveVodTrack(const Destination &d, int live_track)
{
	if (!DestIsTwitch(d))
		return 0;
	int vod = d.vod_track;
	if (vod < 0) {
		size_t mixer = 0;
		if (VodMixer(mixer))
			vod = int(mixer) + 1;
		else
			vod = live_track == 2 ? 1 : 2;
	}
	if (vod < 1 || vod > 6 || vod == live_track)
		return 0;
	return vod;
}

obs_encoder_t *AudioOn(size_t mixer, int kbps)
{
	if (kbps <= 0)
		kbps = 160;
	for (const PooledAudio &p : g_audio) {
		if (p.mixer == mixer && p.kbps == kbps && p.enc)
			return p.enc;
	}
	const QString id = ObsStreamAudioEncoderId();
	obs_data_t *as = AudioSettings(kbps);
	const std::string name = "livedelay_a" + std::to_string(mixer) + "_" + std::to_string(kbps);
	obs_encoder_t *enc = obs_audio_encoder_create(id.toUtf8().constData(), name.c_str(), as, mixer, nullptr);
	obs_data_release(as);
	if (!enc) {
		blog(LOG_ERROR, "[livedelay] could not create audio encoder %s mixer %d", id.toUtf8().constData(),
		     int(mixer) + 1);
		return nullptr;
	}
	obs_encoder_set_audio(enc, obs_get_audio());
	g_audio.push_back(PooledAudio{mixer, kbps, enc});
	return enc;
}

bool BuildOne(DestOutput &o, const Destination &d, size_t index, const char *force_encoder = nullptr)
{
	const bool keep_fallback = o.tried_x264;
	ReleaseOne(o);
	o.tried_x264 = keep_fallback;
	o.name = d.name;
	o.ingest_key = d.ingest_key.empty() ? ("ld" + std::to_string(index)) : d.ingest_key;

	EncodeSettings enc = ResolveEncode(d);
	if (force_encoder)
		enc.video_encoder_id = QString::fromUtf8(force_encoder);
	const size_t liveMixer = ResolveLiveMixer(d);
	const int vodTrack = ResolveVodTrack(d, int(liveMixer) + 1);
	o.audio = AudioOn(liveMixer, enc.audio_kbps);
	if (!o.audio)
		return false;
	obs_encoder_t *vodEnc = nullptr;
	if (vodTrack > 0) {
		vodEnc = AudioOn(size_t(vodTrack - 1), enc.audio_kbps);
		if (!vodEnc)
			blog(LOG_WARNING, "[livedelay] VOD track %d encoder failed for %s", vodTrack, o.name.c_str());
	}

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
	obs_output_set_audio_encoder(o.output, o.audio, 0);

	// Extra video tracks are attached in StartIndex, and only after Twitch returns a
	// GoLive ingest. Multi-video on the classic RTMP url breaks the player (#1000).

	if (vodEnc)
		obs_output_set_audio_encoder(o.output, vodEnc, 1);

	blog(LOG_INFO, "[livedelay] ready %s → %s key=%s (%s %d kb/s) audio=%d%s", o.name.c_str(), server.c_str(),
	     o.ingest_key.c_str(), enc.video_encoder_id.toUtf8().constData(), enc.video_kbps, int(liveMixer) + 1,
	     vodEnc ? (std::string(" vod=") + std::to_string(vodTrack)).c_str() : "");
	return true;
}

bool StartIndex(size_t i);
bool ProxyHasInput();

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

std::string StartFacts(const DestOutput &o, bool input, bool streaming)
{
	std::string s;
	s += input ? "input=yes" : "input=no";
	s += streaming ? " streaming=yes" : " streaming=no";
	s += obs_get_video() ? " mix=yes" : " mix=NO";
	s += obs_get_audio() ? " audio_mix=yes" : " audio_mix=NO";
	if (!o.video) {
		s += " enc=NONE";
	} else {
		const char *id = obs_encoder_get_id(o.video);
		s += " enc=";
		s += id ? id : "?";
		s += obs_encoder_video(o.video) ? " enc_video=yes" : " enc_video=NO";
		if (const char *err = obs_encoder_get_last_error(o.video); err && *err) {
			s += " enc_err=\"";
			s += err;
			s += "\"";
		}
	}
	if (!o.audio)
		s += " aenc=NONE";
	else {
		const char *id = obs_encoder_get_id(o.audio);
		const char *codec = obs_encoder_get_codec(o.audio);
		s += " aenc=";
		s += id ? id : "?";
		s += " aenc_codec=";
		s += codec && *codec ? codec : "NONE";
		s += obs_encoder_audio(o.audio) ? " aenc_audio=yes" : " aenc_audio=NO";
		if (const char *err = obs_encoder_get_last_error(o.audio); err && *err) {
			s += " aenc_err=\"";
			s += err;
			s += "\"";
		}
	}
	const char *url = o.service ? obs_service_get_connect_info(o.service, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : nullptr;
	s += (url && *url) ? " service=yes" : " service=NO";
	s += obs_nv12_tex_active() ? " nv12tex=yes" : " nv12tex=no";
	if (o.output)
		if (const char *err = obs_output_get_last_error(o.output); err && *err) {
			s += " out_err=\"";
			s += err;
			s += "\"";
		}
	return s;
}

void RebindMedia(DestOutput &o)
{
	if (o.video && obs_get_video() && !obs_encoder_video(o.video))
		obs_encoder_set_video(o.video, obs_get_video());
	if (obs_get_audio()) {
		for (PooledAudio &p : g_audio) {
			if (p.enc)
				obs_encoder_set_audio(p.enc, obs_get_audio());
		}
	}
}

bool EncoderAvailable(const char *type)
{
	const char *id = nullptr;
	for (size_t n = 0; obs_enum_encoder_types(n, &id); ++n) {
		if (id && type && std::strcmp(id, type) == 0)
			return true;
	}
	return false;
}

obs_scale_type ScaleOf(const std::string &s)
{
	if (s.find("LANCZOS") != std::string::npos)
		return OBS_SCALE_LANCZOS;
	if (s.find("BILINEAR") != std::string::npos)
		return OBS_SCALE_BILINEAR;
	if (s.find("POINT") != std::string::npos)
		return OBS_SCALE_POINT;
	if (s.find("AREA") != std::string::npos)
		return OBS_SCALE_AREA;
	if (s.find("DISABLE") != std::string::npos)
		return OBS_SCALE_DISABLE;
	return OBS_SCALE_BICUBIC;
}

uint32_t FpsDivisor(const obs_video_info &ovi, uint32_t num, uint32_t den)
{
	if (!num || !den || (ovi.fps_num == num && ovi.fps_den == den))
		return 1;
	const uint64_t target = uint64_t(num) * ovi.fps_den;
	if (!target)
		return 1;
	const uint32_t div = uint32_t((uint64_t(ovi.fps_num) * den) / target);
	return div < 1 ? 1 : div;
}

void PushEgress(size_t index, const std::string &url, const std::string &key)
{
	if (url.empty() || key.empty())
		return;
	QJsonObject body;
	body.insert(QStringLiteral("url"), QString::fromStdString(url));
	body.insert(QStringLiteral("key"), QString::fromStdString(key));
	const std::string json = QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString();
	int status = 0;
	Request("POST", "/dest/target?i=" + std::to_string(index), json, &status);
	if (status != 200)
		blog(LOG_WARNING, "[livedelay] dest/target i=%zu HTTP %d", index, status);
}

bool TryAttachGoLive(DestOutput &o, const Destination &d)
{
	if (!o.output || o.golive)
		return o.golive;
	GoLivePlan plan;
	std::string err;
	if (!FetchGoLive(d, plan, err)) {
		blog(LOG_WARNING,
		     "[livedelay] %s: Enhanced Broadcasting left off (%s). One video track on the normal ingest.",
		     o.name.c_str(), err.c_str());
		return false;
	}
	obs_encoder_group_t *group = obs_encoder_group_create();
	if (!group)
		return false;
	std::vector<obs_encoder_t *> made;
	obs_video_info ovi{};
	obs_get_video_info(&ovi);
	for (size_t n = 0; n < plan.rungs.size() && n < 6; ++n) {
		const GoLiveRung &r = plan.rungs[n];
		if (!EncoderAvailable(r.type.c_str())) {
			blog(LOG_WARNING, "[livedelay] %s: encoder %s is not available", o.name.c_str(), r.type.c_str());
			break;
		}
		obs_data_t *settings = obs_data_create_from_json(r.settings_json.empty() ? "{}" : r.settings_json.c_str());
		if (!settings)
			settings = obs_data_create();
		obs_data_set_bool(settings, "disable_scenecut", true);
		const std::string name = "livedelay_eb_" + o.ingest_key + "_" + std::to_string(n);
		obs_encoder_t *enc = obs_video_encoder_create(r.type.c_str(), name.c_str(), settings, nullptr);
		obs_data_release(settings);
		if (!enc) {
			blog(LOG_WARNING, "[livedelay] %s: failed to create %s", o.name.c_str(), r.type.c_str());
			break;
		}
		obs_encoder_set_video(enc, obs_get_video());
		if (r.width && r.height && (r.width != ovi.output_width || r.height != ovi.output_height)) {
			obs_encoder_set_scaled_size(enc, r.width, r.height);
			obs_encoder_set_gpu_scale_type(enc, ScaleOf(r.scale));
			obs_encoder_set_preferred_video_format(enc, VIDEO_FORMAT_NV12);
		}
		const uint32_t div = FpsDivisor(ovi, r.fps_num, r.fps_den);
		if (div > 1)
			obs_encoder_set_frame_rate_divisor(enc, div);
		if (!obs_encoder_set_group(enc, group)) {
			obs_encoder_release(enc);
			break;
		}
		made.push_back(enc);
	}
	if (made.empty()) {
		obs_encoder_group_destroy(group);
		blog(LOG_WARNING,
		     "[livedelay] %s: Enhanced Broadcasting left off (ladder encoders failed). One video track.",
		     o.name.c_str());
		return false;
	}
	if (o.video)
		obs_encoder_release(o.video);
	o.video = made[0];
	for (size_t n = 1; n < made.size(); ++n)
		o.extra_video.push_back(made[n]);
	o.group = group;
	for (size_t n = 0; n < made.size(); ++n)
		obs_output_set_video_encoder2(o.output, made[n], n);
	o.golive = true;
	o.egress_url = std::move(plan.url);
	o.egress_key = std::move(plan.key);
	blog(LOG_INFO, "[livedelay] %s: Enhanced Broadcasting %zu video tracks", o.name.c_str(), made.size());
	return true;
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
	if (obs_output_active(o.output)) {
		o.want = true;
		return true;
	}
	Settings live = Settings::Load(ConfigPath());
	if (i < live.dests.size()) {
		const Destination &d = live.dests[i];
		const bool wantEb = d.enhanced_broadcasting && DestIsTwitch(d);
		bool retarget = false;
		if (wantEb && !o.golive) {
			retarget = TryAttachGoLive(o, d);
		} else if (!wantEb && o.golive) {
			blog(LOG_INFO, "[livedelay] %s: Enhanced Broadcasting off, one video track", o.name.c_str());
			if (!BuildOne(o, d, i))
				return false;
			retarget = true;
		}
		if (o.golive)
			PushEgress(i, o.egress_url, o.egress_key);
		else if (retarget)
			PushEgress(i, d.url, d.key);
	}
	RebindMedia(o);
	if (o.output && obs_output_start(o.output)) {
		o.want = true;
		blog(LOG_INFO, "[livedelay] started encode for %s%s", o.name.c_str(), o.golive ? " (GoLive)" : "");
		return true;
	}
	if (o.golive && i < live.dests.size()) {
		blog(LOG_WARNING, "[livedelay] %s: GoLive output failed, falling back to one video track", o.name.c_str());
		if (BuildOne(o, live.dests[i], i)) {
			PushEgress(i, live.dests[i].url, live.dests[i].key);
			RebindMedia(o);
			if (o.output && obs_output_start(o.output)) {
				o.want = true;
				blog(LOG_INFO, "[livedelay] started encode for %s", o.name.c_str());
				return true;
			}
		}
	}
	const bool input = ProxyHasInput();
	const std::string facts = StartFacts(o, input, obs_frontend_streaming_active());
	blog(LOG_WARNING, "[livedelay] start %s failed: %s", o.name.c_str(), facts.c_str());
	return false;
}

bool FallbackX264(size_t i)
{
	if (i >= g_outs.size() || g_outs[i].tried_x264)
		return false;
	const char *id = g_outs[i].video ? obs_encoder_get_id(g_outs[i].video) : nullptr;
	if (id && std::string(id) == "obs_x264")
		return false;
	if (!obs_encoder_get_display_name("obs_x264"))
		return false;
	Settings s = Settings::Load(ConfigPath());
	if (i >= s.dests.size())
		return false;
	g_outs[i].tried_x264 = true;
	blog(LOG_WARNING, "[livedelay] %s: retrying on obs_x264 after %s refused", g_outs[i].name.c_str(),
	     id ? id : "?");
	return BuildOne(g_outs[i], s.dests[i], i, "obs_x264");
}

bool ProxyHasInput()
{
	int status = 0;
	const std::string body = Request("GET", "/state", &status);
	return status == 200 && JsonBool(body, "input");
}

void StopArmTimer()
{
	g_arm_pending = false;
	if (g_arm)
		g_arm->stop();
}

void StartReadyPlatforms()
{
	Settings s = Settings::Load(ConfigPath());
	bool any_left = false;
	for (size_t i = 0; i < s.dests.size() && i < g_outs.size(); ++i) {
		Destination &d = s.dests[i];
		const bool ready = d.enabled && !d.url.empty() && !d.key.empty();
		if (!ready && !g_outs[i].want)
			continue;
		if (g_outs[i].want && g_outs[i].output && obs_output_active(g_outs[i].output))
			continue;
		if (!StartIndex(i)) {
			if (FallbackX264(i) && StartIndex(i)) {
				// x264 accepted the output; fall through and arm egress.
			} else {
				any_left = true;
				continue;
			}
		}
		const std::string path = "/dest/start?i=" + std::to_string(i);
		int status = 0;
		Request("POST", path, &status);
		if (status != 200)
			blog(LOG_WARNING, "[livedelay] dest/start i=%zu HTTP %d", i, status);
	}
	if (!any_left)
		StopArmTimer();
}

void OnArmTick()
{
	if (!g_arm_pending) {
		StopArmTimer();
		return;
	}
	g_arm_waited_ms += 250;
	const bool streaming = obs_frontend_streaming_active();
	const bool input = ProxyHasInput();
	if (!streaming && !input) {
		if (g_arm_waited_ms >= 2000) {
			blog(LOG_INFO, "[livedelay] OBS did not stay live; platform start cancelled");
			StopArmTimer();
		}
		return;
	}
	// No picture from OBS yet. Starting an encoder now fails with an empty
	// reason and looks like an NVENC error. Keep waiting while OBS is live.
	if (!input) {
		if ((g_arm_waited_ms >= 4000 && g_arm_noted < 1) || (g_arm_waited_ms >= 12000 && g_arm_noted < 2)) {
			g_arm_noted = g_arm_waited_ms >= 12000 ? 2 : 1;
			blog(LOG_WARNING, "[livedelay] OBS shows streaming but LiveDelay has no video yet (waited %d ms)",
			     g_arm_waited_ms);
		}
		if (g_arm_waited_ms >= 20000) {
			blog(LOG_WARNING, "[livedelay] giving up: no video from OBS after %d ms", g_arm_waited_ms);
			StopArmTimer();
		}
		return;
	}
	StartReadyPlatforms();
	if (g_arm_pending && g_arm_waited_ms >= 20000) {
		blog(LOG_WARNING, "[livedelay] platform start still failing after %d ms", g_arm_waited_ms);
		StopArmTimer();
	}
}

void EnsureArmTimer()
{
	if (g_arm)
		return;
	g_arm = new QTimer;
	g_arm->setInterval(250);
	QObject::connect(g_arm, &QTimer::timeout, g_arm, [] { OnArmTick(); });
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
	StopArmTimer();
	delete g_arm;
	g_arm = nullptr;
	ReleaseAll();
	g_inited = false;
}

void OutputsOnStreamingStarting()
{
	// Last chance before we encode to platforms: stock Multitrack off + key=_idle.
	// Do not start encoders or sleep here. This callback runs before OBS connects
	// to LiveDelay; blocking it is why platform start failed and _idle arrived after.
	HardenObsForLiveDelay();
	if (g_outs.empty())
		Rebuild(false);
	g_arm_pending = true;
	g_arm_waited_ms = 0;
	g_arm_noted = 0;
	EnsureArmTimer();
	blog(LOG_INFO, "[livedelay] OBS is starting — platforms connect after the stream reaches LiveDelay");
	g_arm->start();
}

void OutputsOnStreamingStopping()
{
	StopArmTimer();
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
