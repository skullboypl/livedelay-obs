#pragma once

#include <QString>
#include <QStringList>

namespace livedelay {

// When OBS streams to a "Custom" server (LiveDelay), it stops applying the
// platform's limits, so we do it: the strictest limits of all configured platforms.
struct Limits {
	int video_kbps = 0; // 0 = no limit
	int audio_kbps = 0;
	int keyint_sec = 0;
	QString video_by, audio_by, keyint_by; // platform that sets each limit
};

struct ObsOutput {
	bool advanced = false;
	int video_kbps = 0;
	int audio_kbps = 0;
	int keyint_sec = 0; // 0 = encoder default ("auto")
	QString rate_control; // CBR, VBR, CQP, CRF… (empty in simple mode)
};

Limits PlatformLimits();
ObsOutput ReadObsOutput();
// Human-readable problems, empty when OBS's stream settings fit every platform.
QStringList LimitProblems(const Limits &limits, const ObsOutput &out);
// Writes the limits into the current OBS profile (applies from the next stream
// start; the bitrate also changes right away when the encoder allows it).
bool FitObsOutput(const Limits &limits);

} // namespace livedelay
