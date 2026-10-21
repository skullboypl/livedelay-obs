#include "limits.hpp"
#include "i18n.hpp"
#include "services.hpp"
#include "settings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include <string>

namespace livedelay {

namespace {

void Tighten(int &limit, QString &by, int value, const QString &name)
{
	if (value > 0 && (limit == 0 || value < limit)) {
		limit = value;
		by = name;
	}
}

std::string EncoderJsonPath()
{
	char *dir = obs_frontend_get_current_profile_path();
	std::string path = std::string(dir ? dir : "") + "/streamEncoder.json";
	bfree(dir);
	return path;
}

bool IsAdvanced(config_t *cfg)
{
	const char *mode = config_get_string(cfg, "Output", "Mode");
	return mode && std::string(mode) == "Advanced";
}

} // namespace

Limits PlatformLimits()
{
	Limits l;
	for (const Destination &d : Settings::Load(ConfigPath()).dests) {
		const Service *s = d.service.empty() ? ServiceForUrl(QString::fromStdString(d.url))
						     : FindService(QString::fromStdString(d.service));
		if (!s)
			continue;
		const QString name = QString::fromStdString(d.name);
		Tighten(l.video_kbps, l.video_by, s->max_video_kbps, name);
		Tighten(l.audio_kbps, l.audio_by, s->max_audio_kbps, name);
		Tighten(l.keyint_sec, l.keyint_by, s->keyint_sec, name);
	}
	return l;
}

ObsOutput ReadObsOutput()
{
	ObsOutput o;
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return o;
	o.advanced = IsAdvanced(cfg);
	if (!o.advanced) {
		// Simple mode always uses a 2 s keyframe interval and CBR.
		o.video_kbps = int(config_get_uint(cfg, "SimpleOutput", "VBitrate"));
		o.audio_kbps = int(config_get_uint(cfg, "SimpleOutput", "ABitrate"));
		o.keyint_sec = 2;
		return o;
	}
	obs_data_t *d = obs_data_create_from_json_file_safe(EncoderJsonPath().c_str(), "bak");
	if (d) {
		obs_data_set_default_int(d, "bitrate", 2500);
		obs_data_set_default_string(d, "rate_control", "CBR");
		o.video_kbps = int(obs_data_get_int(d, "bitrate"));
		o.keyint_sec = int(obs_data_get_int(d, "keyint_sec"));
		o.rate_control = QString::fromUtf8(obs_data_get_string(d, "rate_control")).toUpper();
		obs_data_release(d);
	}
	int track = int(config_get_int(cfg, "AdvOut", "TrackIndex"));
	if (track < 1 || track > 6)
		track = 1;
	o.audio_kbps = int(config_get_uint(cfg, "AdvOut", ("Track" + std::to_string(track) + "Bitrate").c_str()));
	return o;
}

QStringList LimitProblems(const Limits &l, const ObsOutput &o)
{
	QStringList p;
	if (l.video_kbps && o.video_kbps > l.video_kbps)
		p << T("Limits.Video").arg(o.video_kbps).arg(l.video_kbps).arg(l.video_by);
	if (l.video_kbps && (o.rate_control == "CQP" || o.rate_control == "CRF" || o.rate_control == "ICQ"))
		p << T("Limits.RateControl").arg(o.rate_control, l.video_by);
	if (l.keyint_sec && (o.keyint_sec == 0 || o.keyint_sec > l.keyint_sec))
		p << T("Limits.Keyint")
			     .arg(o.keyint_sec ? QString::number(o.keyint_sec) + " s" : T("Limits.Auto"))
			     .arg(l.keyint_sec)
			     .arg(l.keyint_by);
	if (l.audio_kbps && o.audio_kbps > l.audio_kbps)
		p << T("Limits.Audio").arg(o.audio_kbps).arg(l.audio_kbps).arg(l.audio_by);
	return p;
}

bool FitObsOutput(const Limits &l)
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return false;
	const ObsOutput now = ReadObsOutput();
	const int video = l.video_kbps && now.video_kbps > l.video_kbps ? l.video_kbps : now.video_kbps;
	const int audio = l.audio_kbps && now.audio_kbps > l.audio_kbps ? l.audio_kbps : now.audio_kbps;
	bool ok = true;
	if (!now.advanced) {
		config_set_uint(cfg, "SimpleOutput", "VBitrate", uint64_t(video));
		config_set_uint(cfg, "SimpleOutput", "ABitrate", uint64_t(audio));
	} else {
		const std::string path = EncoderJsonPath();
		obs_data_t *d = obs_data_create_from_json_file_safe(path.c_str(), "bak");
		if (!d)
			d = obs_data_create();
		obs_data_set_int(d, "bitrate", video);
		if (l.keyint_sec && (now.keyint_sec == 0 || now.keyint_sec > l.keyint_sec))
			obs_data_set_int(d, "keyint_sec", l.keyint_sec);
		if (now.rate_control == "CQP" || now.rate_control == "CRF" || now.rate_control == "ICQ")
			obs_data_set_string(d, "rate_control", "CBR");
		ok = obs_data_save_json_safe(d, path.c_str(), "tmp", "bak");
		obs_data_release(d);
		int track = int(config_get_int(cfg, "AdvOut", "TrackIndex"));
		if (track < 1 || track > 6)
			track = 1;
		config_set_uint(cfg, "AdvOut", ("Track" + std::to_string(track) + "Bitrate").c_str(), uint64_t(audio));
	}
	ok = config_save_safe(cfg, "tmp", nullptr) == 0 && ok;

	// A running stream picks the new bitrate up right away where the encoder allows it.
	if (obs_output_t *out = obs_frontend_get_streaming_output()) {
		if (obs_output_active(out)) {
			if (obs_encoder_t *venc = obs_output_get_video_encoder(out)) {
				obs_data_t *s = obs_data_create();
				obs_data_set_int(s, "bitrate", video);
				obs_encoder_update(venc, s);
				obs_data_release(s);
			}
		}
		obs_output_release(out);
	}
	blog(LOG_INFO, "[livedelay] fitted OBS stream settings: video %d kb/s, audio %d kb/s, keyint %d s", video, audio,
	     l.keyint_sec);
	return ok;
}

} // namespace livedelay
