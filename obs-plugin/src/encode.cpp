#include "encode.hpp"
#include "i18n.hpp"
#include "limits.hpp"
#include "services.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace livedelay {

namespace {

const Service *Svc(const Destination &d)
{
	if (!d.service.empty())
		return FindService(QString::fromStdString(d.service));
	return ServiceForUrl(QString::fromStdString(d.url));
}

void CanvasSize(uint32_t &w, uint32_t &h)
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		w = ovi.base_width;
		h = ovi.base_height;
	} else {
		w = 1920;
		h = 1080;
	}
}

double CanvasFps()
{
	obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_den)
		return double(ovi.fps_num) / double(ovi.fps_den);
	return 30.0;
}

// Twitch-style guideline table when Auto has no API (v1).
int AutoVideoKbps(const Service *s, uint32_t w, uint32_t h, double fps)
{
	const int pixels = int(w * h);
	const bool hi = fps >= 50.0;
	int guide = 2500;
	if (pixels >= 1920 * 1080)
		guide = hi ? 6000 : 4500;
	else if (pixels >= 1280 * 720)
		guide = hi ? 4500 : 3000;
	else
		guide = hi ? 2500 : 1500;
	if (s && s->max_video_kbps > 0)
		guide = std::min(guide, s->max_video_kbps);
	return guide;
}

QString SimpleEncoderId(const char *name)
{
	if (!name)
		return "obs_x264";
	std::string e = name;
	if (e == "x264" || e == "x264_lowcpu")
		return "obs_x264";
	if (e == "qsv")
		return "obs_qsv11_v2";
	if (e == "qsv_av1")
		return "obs_qsv11_av1";
	if (e == "amd")
		return "h264_texture_amf";
	if (e == "amd_hevc")
		return "h265_texture_amf";
	if (e == "amd_av1")
		return "av1_texture_amf";
	if (e == "nvenc")
		return obs_encoder_get_display_name("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex" : "ffmpeg_nvenc";
	if (e == "nvenc_hevc")
		return "obs_nvenc_hevc_tex";
	if (e == "nvenc_av1")
		return "obs_nvenc_av1_tex";
	// Already a real encoder id (advanced mode stores those).
	if (obs_encoder_get_display_name(name))
		return QString::fromUtf8(name);
	return "obs_x264";
}

} // namespace

EncodeMode ParseEncodeMode(const std::string &s)
{
	std::string v = s;
	for (char &c : v)
		c = char(tolower(static_cast<unsigned char>(c)));
	if (v == "auto")
		return EncodeMode::Auto;
	if (v == "recommended" || v == "zalecane")
		return EncodeMode::Recommended;
	if (v == "twitch_partner" || v == "partner" || v == "zalecane_partner")
		return EncodeMode::TwitchPartner;
	if (v == "manual" || v == "recznie" || v == "ręcznie")
		return EncodeMode::Manual;
	return EncodeMode::Obs;
}

const char *EncodeModeKey(EncodeMode m)
{
	switch (m) {
	case EncodeMode::Auto:
		return "auto";
	case EncodeMode::Recommended:
		return "recommended";
	case EncodeMode::TwitchPartner:
		return "twitch_partner";
	case EncodeMode::Manual:
		return "manual";
	case EncodeMode::Obs:
	default:
		return "obs";
	}
}

QString ObsStreamVideoEncoderId()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return "obs_x264";
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (mode && std::string(mode) == "Advanced") {
		const char *id = config_get_string(cfg, "AdvOut", "Encoder");
		return id && *id ? QString::fromUtf8(id) : QString("obs_x264");
	}
	return SimpleEncoderId(config_get_string(cfg, "SimpleOutput", "StreamEncoder"));
}

bool NeedsClassicH264(const Destination &d)
{
	const QString svc = QString::fromStdString(d.service).toLower();
	const QString url = QString::fromStdString(d.url).toLower();
	return svc.contains("twitch") || url.contains("twitch.tv") || url.contains("ttvnw.net") ||
	       url.contains("live-video.net") || url.contains("contribute.live-video.net");
}

// Classic Twitch RTMP rejects HEVC/AV1 → player error #1000. Map to H.264 sibling.
QString ForceClassicH264(const QString &id)
{
	const QString l = id.toLower();
	if (!(l.contains("hevc") || l.contains("h265") || l.contains("av1")))
		return id;
	if (l.contains("nvenc"))
		return obs_encoder_get_display_name("obs_nvenc_h264_tex") ? QStringLiteral("obs_nvenc_h264_tex")
									 : QStringLiteral("ffmpeg_nvenc");
	if (l.contains("amf") || l.contains("amd"))
		return QStringLiteral("h264_texture_amf");
	if (l.contains("qsv"))
		return QStringLiteral("obs_qsv11_v2");
	return QStringLiteral("obs_x264");
}

// True when `id` is a registered encoder of `type`. OBS still returns an
// encoder object for an unknown id, and that object can never start.
bool RealEncoder(const char *id, obs_encoder_type type)
{
	return id && *id && obs_get_encoder_codec(id) && obs_get_encoder_type(id) == type;
}

QString FirstAudioEncoder(std::initializer_list<const char *> prefer)
{
	for (const char *id : prefer)
		if (RealEncoder(id, OBS_ENCODER_AUDIO))
			return QString::fromUtf8(id);
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i)
		if (RealEncoder(id, OBS_ENCODER_AUDIO) && std::strcmp(obs_get_encoder_codec(id), "aac") == 0)
			return QString::fromUtf8(id);
	return QStringLiteral("ffmpeg_aac");
}

QString ObsStreamAudioEncoderId()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return FirstAudioEncoder({"ffmpeg_aac", "libfdk_aac", "CoreAudio_AAC"});
	const char *mode = config_get_string(cfg, "Output", "Mode");
	const bool advanced = mode && std::string(mode) == "Advanced";
	const char *raw = advanced ? config_get_string(cfg, "AdvOut", "AudioEncoder")
				   : config_get_string(cfg, "SimpleOutput", "StreamAudioEncoder");
	// Simple mode stores the codec name ("aac" / "opus"), not an encoder id.
	// Passing "aac" to obs_audio_encoder_create builds a dead encoder, so every
	// platform start fails after the video encoder with no error text.
	if (!advanced && raw && std::strcmp(raw, "opus") == 0)
		return FirstAudioEncoder({"ffmpeg_opus"});
	if (RealEncoder(raw, OBS_ENCODER_AUDIO))
		return QString::fromUtf8(raw);
	const QString resolved = FirstAudioEncoder({"ffmpeg_aac", "libfdk_aac", "CoreAudio_AAC"});
	blog(LOG_INFO, "[livedelay] audio setting \"%s\" is not an encoder id, using %s", raw ? raw : "",
	     resolved.toUtf8().constData());
	return resolved;
}

EncodeSettings ResolveEncode(const Destination &d)
{
	EncodeSettings e;
	e.video_encoder_id = ObsStreamVideoEncoderId();
	const ObsOutput obs = ReadObsOutput();
	const Service *s = Svc(d);
	uint32_t cw = 0, ch = 0;
	CanvasSize(cw, ch);

	switch (d.encode_mode) {
	case EncodeMode::Obs:
		e.video_kbps = obs.video_kbps > 0 ? obs.video_kbps : 2500;
		e.audio_kbps = obs.audio_kbps > 0 ? obs.audio_kbps : 160;
		e.keyint_sec = obs.keyint_sec > 0 ? obs.keyint_sec : 2;
		e.rate_control = obs.rate_control.isEmpty() ? "CBR" : obs.rate_control;
		e.summary = T("Encode.SummaryObs")
				    .arg(e.video_kbps)
				    .arg(e.rate_control)
				    .arg(e.keyint_sec)
				    .arg(e.video_encoder_id);
		break;
	case EncodeMode::Recommended: {
		e.video_kbps = s && s->max_video_kbps > 0 ? s->max_video_kbps : (obs.video_kbps > 0 ? obs.video_kbps : 6000);
		e.audio_kbps = s && s->max_audio_kbps > 0 ? s->max_audio_kbps : 160;
		e.keyint_sec = s && s->keyint_sec > 0 ? s->keyint_sec : 2;
		e.rate_control = "CBR";
		e.summary = T("Encode.SummaryRecommended")
				    .arg(e.video_kbps)
				    .arg(e.keyint_sec)
				    .arg(s ? s->name : QString::fromStdString(d.name));
		break;
	}
	case EncodeMode::TwitchPartner: {
		// Partner / Affiliate classic RTMP: Twitch still caps ~6000, but we pick
		// the strongest stable profile (1080p60 CBR, keyint 2, high audio).
		const double fps = CanvasFps();
		const bool hi = fps >= 50.0;
		e.video_kbps = hi ? 6000 : 4500;
		if (cw * ch < 1280 * 720)
			e.video_kbps = hi ? 4500 : 3000;
		if (s && s->max_video_kbps > 0)
			e.video_kbps = std::min(e.video_kbps, s->max_video_kbps);
		e.audio_kbps = 320;
		if (s && s->max_audio_kbps > 0)
			e.audio_kbps = std::min(e.audio_kbps, s->max_audio_kbps);
		e.keyint_sec = 2;
		e.rate_control = "CBR";
		e.summary = T("Encode.SummaryTwitchPartner").arg(e.video_kbps).arg(e.audio_kbps);
		break;
	}
	case EncodeMode::Auto: {
		e.video_kbps = AutoVideoKbps(s, cw, ch, CanvasFps());
		e.audio_kbps = s && s->max_audio_kbps > 0 ? std::min(160, s->max_audio_kbps) : 160;
		e.keyint_sec = s && s->keyint_sec > 0 ? s->keyint_sec : 2;
		e.rate_control = "CBR";
		e.summary = T("Encode.SummaryAuto").arg(e.video_kbps).arg(cw).arg(ch).arg(int(std::lround(CanvasFps())));
		break;
	}
	case EncodeMode::Manual:
		e.video_kbps = d.video_kbps > 0 ? int(d.video_kbps) : (obs.video_kbps > 0 ? obs.video_kbps : 2500);
		e.audio_kbps = d.audio_kbps > 0 ? int(d.audio_kbps) : 160;
		e.keyint_sec = d.keyint_sec > 0 ? int(d.keyint_sec) : 2;
		e.rate_control = d.rate_control.empty() ? "CBR" : QString::fromStdString(d.rate_control);
		e.width = d.width;
		e.height = d.height;
		e.summary = T("Encode.SummaryManual").arg(e.video_kbps).arg(e.rate_control).arg(e.keyint_sec);
		break;
	}
	if (NeedsClassicH264(d)) {
		const QString was = e.video_encoder_id;
		e.video_encoder_id = ForceClassicH264(e.video_encoder_id);
		if (e.video_encoder_id != was) {
			blog(LOG_WARNING,
			     "[livedelay] %s: classic Twitch RTMP needs H.264 — mapped %s → %s (HEVC/AV1 → player #1000)",
			     d.name.c_str(), was.toUtf8().constData(), e.video_encoder_id.toUtf8().constData());
			e.summary += " · H.264";
		}
		// Classic Twitch ingest (no Enhanced Broadcasting): >1080p often
		// shows LIVE with bitrate but player error #1000 for viewers.
		// Manual width/height still honored when both are set.
		const bool manual_size = d.encode_mode == EncodeMode::Manual && d.width > 0 && d.height > 0;
		uint32_t out_w = e.width > 0 ? e.width : cw;
		uint32_t out_h = e.height > 0 ? e.height : ch;
		if (!d.enhanced_broadcasting && !manual_size && (out_w > 1920 || out_h > 1080)) {
			const double scale = std::min(1920.0 / double(out_w), 1080.0 / double(out_h));
			e.width = uint32_t(std::lround(out_w * scale)) & ~1u;
			e.height = uint32_t(std::lround(out_h * scale)) & ~1u;
			if (e.width < 2)
				e.width = 1920;
			if (e.height < 2)
				e.height = 1080;
			blog(LOG_INFO, "[livedelay] %s: scaling %ux%u → %ux%u for classic Twitch RTMP", d.name.c_str(),
			     out_w, out_h, e.width, e.height);
			e.summary += QStringLiteral(" · %1x%2").arg(e.width).arg(e.height);
		}
	}
	if (d.audio_track >= 1 && d.audio_track <= 6)
		e.summary += QStringLiteral(" · ") + T("Encode.TrackN").arg(d.audio_track);
	if (d.vod_track > 0 || d.vod_track < 0 || d.vod_audio)
		e.summary += QStringLiteral(" · ") + T("Encode.SummaryVodOn");
	if (d.enhanced_broadcasting)
		e.summary += QStringLiteral(" · ") + T("Encode.SummaryEnhancedOn");
	return e;
}

QStringList EncodeProblems()
{
	QStringList p;
	const Settings s = Settings::Load(ConfigPath());
	const ObsOutput obs = ReadObsOutput();
	Limits tight;
	QString video_by, audio_by, keyint_by;
	int n_obs = 0;
	for (const Destination &d : s.dests) {
		if (!d.enabled && d.encode_mode != EncodeMode::Obs)
			continue;
		if (d.encode_mode != EncodeMode::Obs)
			continue;
		++n_obs;
		const Service *svc = Svc(d);
		if (!svc)
			continue;
		const QString name = QString::fromStdString(d.name);
		if (svc->max_video_kbps > 0 && (tight.video_kbps == 0 || svc->max_video_kbps < tight.video_kbps)) {
			tight.video_kbps = svc->max_video_kbps;
			video_by = name;
		}
		if (svc->max_audio_kbps > 0 && (tight.audio_kbps == 0 || svc->max_audio_kbps < tight.audio_kbps)) {
			tight.audio_kbps = svc->max_audio_kbps;
			audio_by = name;
		}
		if (svc->keyint_sec > 0 && (tight.keyint_sec == 0 || svc->keyint_sec < tight.keyint_sec)) {
			tight.keyint_sec = svc->keyint_sec;
			keyint_by = name;
		}
	}
	tight.video_by = video_by;
	tight.audio_by = audio_by;
	tight.keyint_by = keyint_by;
	if (n_obs == 0)
		return p;
	p = LimitProblems(tight, obs);
	if (!p.isEmpty())
		p.prepend(T("Limits.ObsModeHint"));
	return p;
}

bool FitEncodeForObsMode()
{
	const Settings s = Settings::Load(ConfigPath());
	Limits tight;
	for (const Destination &d : s.dests) {
		if (d.encode_mode != EncodeMode::Obs)
			continue;
		const Service *svc = Svc(d);
		if (!svc)
			continue;
		const QString name = QString::fromStdString(d.name);
		if (svc->max_video_kbps > 0 && (tight.video_kbps == 0 || svc->max_video_kbps < tight.video_kbps)) {
			tight.video_kbps = svc->max_video_kbps;
			tight.video_by = name;
		}
		if (svc->max_audio_kbps > 0 && (tight.audio_kbps == 0 || svc->max_audio_kbps < tight.audio_kbps)) {
			tight.audio_kbps = svc->max_audio_kbps;
			tight.audio_by = name;
		}
		if (svc->keyint_sec > 0 && (tight.keyint_sec == 0 || svc->keyint_sec < tight.keyint_sec)) {
			tight.keyint_sec = svc->keyint_sec;
			tight.keyint_by = name;
		}
	}
	return FitObsOutput(tight);
}

QString MultiEncodeInfo()
{
	const int n = int(Settings::Load(ConfigPath()).dests.size());
	if (n >= 3)
		return T("Limits.MultiEncodeInfo").arg(n);
	return {};
}

} // namespace livedelay
