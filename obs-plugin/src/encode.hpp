#pragma once

#include "settings.hpp"

#include <QString>
#include <QStringList>

namespace livedelay {

// Resolved encoder settings for one platform (applied when starting its output).
struct EncodeSettings {
	QString video_encoder_id; // obs_nvenc_h264_tex, obs_x264, …
	int video_kbps = 2500;
	int audio_kbps = 160;
	int keyint_sec = 2;
	QString rate_control = "CBR";
	uint32_t width = 0;  // 0 = canvas
	uint32_t height = 0;
	QString summary; // human-readable for the settings UI
};

EncodeMode ParseEncodeMode(const std::string &s);
const char *EncodeModeKey(EncodeMode m);

// Fills EncodeSettings from encode_mode + OBS Output + services.json / guidelines.
EncodeSettings ResolveEncode(const Destination &d);

// Problems when OBS Output (used by mode "obs") exceeds platform limits for
// destinations that are in obs mode — shown in the dock banner.
QStringList EncodeProblems();

// Fits OBS → Output to the strictest limits among dests in mode "obs"
// (and any that would use OBS as a template). Returns false on write failure.
bool FitEncodeForObsMode();

// Soft tip when many separate encoders will run (informational banner).
QString MultiEncodeInfo();

// Encoder id OBS itself uses for streaming (simple or advanced).
QString ObsStreamVideoEncoderId();
QString ObsStreamAudioEncoderId();

} // namespace livedelay
