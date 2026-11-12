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
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace livedelay {

bool IsKickTarget(const QString &service, const QString &url)
{
	// Twitch's automatic ingest is ingest.global-contribute.live-video.net.
	// Treating that host as Kick rewrote a Twitch HEVC encode to H.264 NVENC.
	if (service.contains(QStringLiteral("twitch"), Qt::CaseInsensitive))
		return false;
	if (url.contains(QStringLiteral("twitch.tv"), Qt::CaseInsensitive) ||
	    url.contains(QStringLiteral("ttvnw.net"), Qt::CaseInsensitive) ||
	    url.contains(QStringLiteral("ingest.global-contribute.live-video.net"), Qt::CaseInsensitive))
		return false;
	if (service.contains(QStringLiteral("kick"), Qt::CaseInsensitive) ||
	    url.contains(QStringLiteral("kick.com"), Qt::CaseInsensitive))
		return true;
	// Kick's dashboard URL is <id>.global-contribute.live-video.net, not Twitch's "ingest." host.
	return url.contains(QStringLiteral("global-contribute.live-video.net"), Qt::CaseInsensitive) &&
	       !url.contains(QStringLiteral("ingest.global-contribute"), Qt::CaseInsensitive);
}

bool IsTwitchTarget(const QString &service, const QString &url)
{
	if (IsKickTarget(service, url))
		return false;
	return service.contains(QStringLiteral("twitch"), Qt::CaseInsensitive) ||
	       url.contains(QStringLiteral("twitch"), Qt::CaseInsensitive) ||
	       url.contains(QStringLiteral("ttvnw.net"), Qt::CaseInsensitive) ||
	       url.contains(QStringLiteral("live-video.net"), Qt::CaseInsensitive);
}

bool SupportsSeparateVodTrack(const QString &service, const QString &url)
{
	return IsTwitchTarget(service, url);
}

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

bool NamedKick(const Destination &d, const Service *s)
{
	if (IsKickTarget(QString::fromStdString(d.service), QString::fromStdString(d.url)))
		return true;
	return s && s->name.contains(QStringLiteral("kick"), Qt::CaseInsensitive);
}

// Twitch's non-partner cap. Kick is excluded first: its ingest is also on live-video.net.
bool NamedTwitch(const Destination &d, const Service *s)
{
	if (NamedKick(d, s))
		return false;
	if (IsTwitchTarget(QString::fromStdString(d.service), QString::fromStdString(d.url)))
		return true;
	return s && s->name.contains(QStringLiteral("twitch"), Qt::CaseInsensitive);
}

// What "Zalecane" encodes. Twitch without Partner is 6000. Kick's own baseline
// is 8000. Everyone else uses that service's recommended max.
int RecommendedVideoKbps(const Destination &d, const Service *s)
{
	if (NamedKick(d, s))
		return 8000;
	if (NamedTwitch(d, s))
		return 6000;
	if (s && s->max_video_kbps > 0)
		return s->max_video_kbps;
	return 6000;
}

// Twitch-style guideline table when Auto has no API (v1).
int AutoVideoKbps(const Destination &d, const Service *s, uint32_t w, uint32_t h, double fps)
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
	// Kick allows 8000 at 1080p60. The table above is Twitch's, so lift only
	// that case; smaller Kick canvases stay on the resolution table.
	if (NamedKick(d, s) && pixels >= 1920 * 1080)
		guide = hi ? 8000 : 6000;
	else if (s && s->max_video_kbps > 0)
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
	return IsTwitchTarget(QString::fromStdString(d.service), QString::fromStdString(d.url));
}

// Same GPU family. Jumping from NVENC HEVC to x264 is the single-PC FPS hit.
bool SameEncoderFamily(const QString &a, const QString &b)
{
	auto family = [](QString s) {
		s = s.toLower();
		if (s.contains(QStringLiteral("nvenc")))
			return 1;
		if (s.contains(QStringLiteral("amf")) || s.contains(QStringLiteral("amd")))
			return 2;
		if (s.contains(QStringLiteral("qsv")))
			return 3;
		return 4;
	};
	return family(a) == family(b);
}

// Stay on the encoder OBS already uses. A missing AV1/HEVC codec keeps `id`
// instead of falling back to x264.
QString CodecOnSameEncoder(const QString &id, const char *nvenc, const char *amf, const char *qsv)
{
	const QString l = id.toLower();
	const char *want = nullptr;
	if (l.contains(QStringLiteral("nvenc")))
		want = nvenc;
	else if (l.contains(QStringLiteral("amf")) || l.contains(QStringLiteral("amd")))
		want = amf;
	else if (l.contains(QStringLiteral("qsv")))
		want = qsv;
	else if (std::string(nvenc).find("h264") != std::string::npos && obs_encoder_get_display_name("obs_x264"))
		return QStringLiteral("obs_x264");
	if (!want || !obs_encoder_get_display_name(want))
		return id;
	return QString::fromUtf8(want);
}

QString ApplyBufferCodec(const QString &id, const std::string &codec)
{
	if (codec.empty() || codec == "obs")
		return id;
	if (codec == "h264")
		return CodecOnSameEncoder(id, "obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2");
	if (codec == "hevc")
		return CodecOnSameEncoder(id, "obs_nvenc_hevc_tex", "h265_texture_amf", "obs_qsv11_hevc");
	if (codec == "av1")
		return CodecOnSameEncoder(id, "obs_nvenc_av1_tex", "av1_texture_amf", "obs_qsv11_av1");
	return id;
}

const char *SimpleEncoderName(const QString &id)
{
	const QString l = id.toLower();
	if (l.contains(QStringLiteral("nvenc"))) {
		if (l.contains(QStringLiteral("av1")))
			return "nvenc_av1";
		if (l.contains(QStringLiteral("hevc")) || l.contains(QStringLiteral("h265")))
			return "nvenc_hevc";
		return "nvenc";
	}
	if (l.contains(QStringLiteral("amf")) || l.contains(QStringLiteral("amd"))) {
		if (l.contains(QStringLiteral("av1")))
			return "amd_av1";
		if (l.contains(QStringLiteral("hevc")) || l.contains(QStringLiteral("265")))
			return "amd_hevc";
		return "amd";
	}
	if (l.contains(QStringLiteral("qsv"))) {
		if (l.contains(QStringLiteral("av1")))
			return "qsv_av1";
		if (l.contains(QStringLiteral("hevc")) || l.contains(QStringLiteral("265")))
			return "qsv_hevc";
		return "qsv";
	}
	return "x264";
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

void SyncObsEncoderToBufferCodec()
{
	const Settings s = Settings::Load(ConfigPath());
	if (!s.single_pc || s.buffer_codec.empty() || s.buffer_codec == "obs")
		return;
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return;
	const QString cur = ObsStreamVideoEncoderId();
	const QString next = ApplyBufferCodec(cur, s.buffer_codec);
	if (next == cur || !SameEncoderFamily(cur, next))
		return;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (mode && std::string(mode) == "Advanced")
		config_set_string(cfg, "AdvOut", "Encoder", next.toUtf8().constData());
	else
		config_set_string(cfg, "SimpleOutput", "StreamEncoder", SimpleEncoderName(next));
	config_save_safe(cfg, "tmp", nullptr);
	blog(LOG_INFO, "[livedelay] single-PC buffer uses one OBS encoder: %s", next.toUtf8().constData());
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

// The other enabled platforms on the same input key: since 1.0.41 they take
// the encode of the first one instead of having their own.
static std::vector<const Destination *> Sharers(const Settings &s, const Destination &d)
{
	std::vector<const Destination *> out;
	if (d.ingest_key.empty())
		return out;
	for (const Destination &x : s.dests)
		if (x.enabled && x.ingest_key == d.ingest_key && x.name != d.name)
			out.push_back(&x);
	return out;
}

Destination SharedEncodeView(const Settings &s, const Destination &d)
{
	Destination v = d;
	for (const Destination *x : Sharers(s, d)) {
		const QString svc = QString::fromStdString(x->service), url = QString::fromStdString(x->url);
		// Enhanced Broadcasting changes the encode itself (Twitch's bitrate
		// ladder), so a non-Twitch sharer turns it off for everyone.
		if (!IsTwitchTarget(svc, url))
			v.enhanced_broadcasting = false;
		// Twitch's VOD track stays: the engine drops the second AAC before it
		// reaches Kick and the others (Dest::single_track), so Twitch keeps
		// its VOD audio on a shared input. When the encode belongs to Kick
		// (listed first), it takes the Twitch sharer's VOD choice.
		const bool ownVod = SupportsSeparateVodTrack(QString::fromStdString(d.service),
							     QString::fromStdString(d.url)) || d.vod_override;
		if (!ownVod && SupportsSeparateVodTrack(svc, url) && (x->vod_track != 0 || x->vod_audio)) {
			v.vod_track = x->vod_track;
			v.vod_track_set = x->vod_track_set;
			v.vod_audio = x->vod_audio;
			v.vod_override = true;
		}
	}
	return v;
}

Destination SharedEncodeView(const Destination &d)
{
	return SharedEncodeView(Settings::Load(ConfigPath()), d);
}

EncodeSettings ResolveEncode(const Destination &own)
{
	EncodeSettings e;
	const Settings cfg = Settings::Load(ConfigPath());
	const Destination d = SharedEncodeView(cfg, own);
	bool sharerKick = false, sharerTwitch = false;
	for (const Destination *x : Sharers(cfg, own)) {
		const QString svc = QString::fromStdString(x->service), url = QString::fromStdString(x->url);
		sharerKick = sharerKick || IsKickTarget(svc, url);
		sharerTwitch = sharerTwitch || IsTwitchTarget(svc, url);
	}
	e.video_encoder_id = ApplyBufferCodec(ObsStreamVideoEncoderId(), cfg.buffer_codec);
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
		e.video_kbps = RecommendedVideoKbps(d, s);
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
		e.video_kbps = AutoVideoKbps(d, s, cw, ch, CanvasFps());
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
	// Twitch's 1080p cap and Kick's H.264 also hold when that platform only
	// shares this encode.
	const bool classicTwitch = NeedsClassicH264(d) || sharerTwitch;
	// Kick's ingest is H.264 only. Apply that only when this destination is
	// Kick and it is actually enabled. A Twitch-only stream keeps the OBS
	// codec, so NVENC is not switched from HEVC to H.264 behind the user's back.
	// An explicit buffer codec wins over that default.
	const bool kickLive = cfg.buffer_codec == "obs" &&
			      ((d.enabled && IsKickTarget(QString::fromStdString(d.service), QString::fromStdString(d.url))) ||
			       sharerKick);
	if (kickLive) {
		const QString was = e.video_encoder_id;
		const QString h264 = ForceClassicH264(was);
		if (h264 != was && SameEncoderFamily(was, h264)) {
			e.video_encoder_id = h264;
			e.summary += QStringLiteral(" · H.264 (Kick)");
		}
	}
	if (classicTwitch || kickLive) {
		// Classic Twitch ingest (no Enhanced Broadcasting): >1080p often
		// shows LIVE with bitrate but player error #1000 for viewers.
		// Kick is not capped here. Manual width/height still honored when both are set.
		const bool manual_size = d.encode_mode == EncodeMode::Manual && d.width > 0 && d.height > 0;
		uint32_t out_w = e.width > 0 ? e.width : cw;
		uint32_t out_h = e.height > 0 ? e.height : ch;
		if (classicTwitch && !d.enhanced_broadcasting && !manual_size && (out_w > 1920 || out_h > 1080)) {
			const double scale = std::min(1920.0 / double(out_w), 1080.0 / double(out_h));
			e.width = uint32_t(std::lround(out_w * scale)) & ~1u;
			e.height = uint32_t(std::lround(out_h * scale)) & ~1u;
			if (e.width < 2)
				e.width = 1920;
			if (e.height < 2)
				e.height = 1080;
			// The dock and the settings window ask for this every few seconds,
			// so the log says it once per platform and size, not every time.
			static std::mutex logged_lock;
			static std::map<std::string, std::string> logged;
			const std::string what = std::to_string(out_w) + "x" + std::to_string(out_h) + ">" +
						 std::to_string(e.width) + "x" + std::to_string(e.height);
			bool say = false;
			{
				std::lock_guard<std::mutex> g(logged_lock);
				std::string &last = logged[d.name];
				say = last != what;
				last = what;
			}
			if (say)
				blog(LOG_INFO, "[livedelay] %s: scaling %ux%u → %ux%u for classic Twitch RTMP", d.name.c_str(),
				     out_w, out_h, e.width, e.height);
			e.summary += QStringLiteral(" · %1x%2").arg(e.width).arg(e.height);
		}
	}
	if (d.audio_track >= 1 && d.audio_track <= 6)
		e.summary += QStringLiteral(" · ") + T("Encode.TrackN").arg(d.audio_track);
	if ((classicTwitch || d.vod_override) && (d.vod_track > 0 || d.vod_track < 0 || d.vod_audio))
		e.summary += QStringLiteral(" · ") + T("Encode.SummaryVodOn");
	if (classicTwitch && d.enhanced_broadcasting)
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

QString TwitchCapInfo()
{
	const Settings s = Settings::Load(ConfigPath());
	if (!s.single_pc && !s.obs_live)
		return {};
	bool twitch = false;
	for (const Destination &d : s.dests) {
		const Input *in = s.FindInput(d.input);
		const bool by_hand = d.encode_mode == EncodeMode::Manual && d.width > 0 && d.height > 0;
		twitch = twitch || (d.enabled && !(in && in->external) && !by_hand &&
				    IsTwitchTarget(QString::fromStdString(d.service), QString::fromStdString(d.url)));
	}
	obs_video_info ovi{};
	if (!twitch || !obs_get_video_info(&ovi) || (ovi.output_width <= 1920 && ovi.output_height <= 1080))
		return {};
	const double scale = std::min(1920.0 / double(ovi.output_width), 1080.0 / double(ovi.output_height));
	return T("Limits.TwitchCapInfo")
		.arg(ovi.output_width)
		.arg(ovi.output_height)
		.arg(uint32_t(std::lround(ovi.output_width * scale)) & ~1u)
		.arg(uint32_t(std::lround(ovi.output_height * scale)) & ~1u);
}

QString MultiEncodeInfo()
{
	const Settings s = Settings::Load(ConfigPath());
	EncodeSettings first;
	bool have = false;
	bool differ = false;
	int n = 0;
	std::vector<std::string> seen;
	for (const Destination &d : s.dests) {
		if (!d.enabled)
			continue;
		if (const Input *in = s.FindInput(d.input)) {
			if (in->external || std::find(seen.begin(), seen.end(), in->key) != seen.end())
				continue;
			seen.push_back(in->key);
		}
		++n;
		if (d.enhanced_broadcasting &&
		    IsTwitchTarget(QString::fromStdString(d.service), QString::fromStdString(d.url)))
			differ = true;
		const EncodeSettings e = ResolveEncode(d);
		if (!have) {
			first = e;
			have = true;
			continue;
		}
		if (e.video_encoder_id != first.video_encoder_id || e.video_kbps != first.video_kbps ||
		    e.keyint_sec != first.keyint_sec || e.rate_control != first.rate_control || e.width != first.width ||
		    e.height != first.height)
			differ = true;
	}
	// Three matching platforms still each have an encoder, but the settings are one
	// profile. The note is for the case where those profiles actually diverge.
	if (n >= 3 && differ)
		return T("Limits.MultiEncodeInfo");
	return {};
}

} // namespace livedelay
