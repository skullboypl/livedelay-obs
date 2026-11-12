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

// Twitch auto-ingest is ingest.global-contribute.live-video.net. That host is
// not Kick. Kick is the service name, kick.com, or another tenant on that host.
bool IsKickTarget(const QString &service, const QString &url);
bool IsTwitchTarget(const QString &service, const QString &url);
// Twitch ingest stores a second AAC as the VOD mix. Other RTMP platforms do not.
bool SupportsSeparateVodTrack(const QString &service, const QString &url);

// Fills EncodeSettings from encode_mode + OBS Output + services.json / guidelines.
EncodeSettings ResolveEncode(const Destination &d);
// `d` as the encode it owns must be: without Enhanced Broadcasting or the VOD
// track when another platform on the same input cannot take them.
Destination SharedEncodeView(const Destination &d);

// Problems when OBS Output (used by mode "obs") exceeds platform limits for
// destinations that are in obs mode — shown in the dock banner.
QStringList EncodeProblems();
// Single PC / force live with an OBS output above 1080p and a Twitch platform:
// says that the stream is sent smaller, and why. Empty when it does not apply.
QString TwitchCapInfo();

// Fits OBS → Output to the strictest limits among dests in mode "obs"
// (and any that would use OBS as a template). Returns false on write failure.
bool FitEncodeForObsMode();

// Soft tip when three or more platforms resolve to different encode settings.
QString MultiEncodeInfo();

// Encoder id OBS itself uses for streaming (simple or advanced).
QString ObsStreamVideoEncoderId();
QString ObsStreamAudioEncoderId();

// One-PC mode: the buffer codec is OBS's own stream encoder. Writes the profile
// when the user picked H.264, HEVC or AV1, and stays on the same GPU vendor.
void SyncObsEncoderToBufferCodec();

} // namespace livedelay
