#pragma once

#include <cstdint>
#include <string>
#include <vector>

class QWidget;

namespace livedelay {

enum class EncodeMode { Auto, Recommended, TwitchPartner, Obs, Manual };

// livedelay.ini as the settings window sees it. The proxy reads the same file.
struct Destination {
	std::string name, url, key;
	bool enabled = true;
	bool autostart = true;
	std::string service; // name from OBS's service list, empty = custom server
	std::string ingest_key; // RTMP key into LiveDelay (ld0, …)
	EncodeMode encode_mode = EncodeMode::Obs;
	uint32_t video_kbps = 0;
	uint32_t audio_kbps = 0;
	uint32_t keyint_sec = 0;
	std::string rate_control = "CBR";
	uint32_t width = 0;
	uint32_t height = 0;
	/// Second AAC track for Twitch VOD (mixer from OBS AdvOut VodTrackIndex).
	bool vod_audio = false;
	/// Multiple video encodes (Partner ladder / Enhanced Broadcasting path).
	bool enhanced_broadcasting = false;
};

struct Settings {
	std::string listen = "127.0.0.1:1935";
	std::string api = "127.0.0.1:7799";
	int delay = 15;
	int max_buffer_mb = 512;
	/// Seconds for the six "arm this delay" hotkeys. Keys themselves live in OBS.
	int arm_slot[6] = {15, 30, 45, 60, 90, 120};
	std::string language = "auto"; // auto = follow OBS, or en-US / pl-PL / de-DE / ru-RU
	std::string overlay_live = "LIVE";
	std::string overlay_arming = "ARMING… {buf}/{s}s";
	std::string overlay_armed = "ARMED {s}s";
	std::string overlay_delayed = "DELAY {s}s";
	std::vector<Destination> dests;

	static Settings Load(const std::string &path);
	bool Save(const std::string &path) const;
	std::string Text() const; // the ini as written by Save()
	int RtmpPort() const;
};

// Path of the ini inside OBS's plugin config folder (created if missing).
std::string ConfigPath();
// (Re)starts the embedded proxy. Returns an empty string or the error.
std::string StartProxy();

std::string OverlayUrl();
std::string LivePreviewUrl();
// True when OBS's stream settings point at LiveDelay.
bool ObsRoutedToLiveDelay();
// If already routed: turn off stock OBS Enhanced Broadcasting (Multitrack) and
// enable custom-server VOD track support. Safe to call on every OBS load.
void HardenObsForLiveDelay();
// Moves OBS's current server and key onto LiveDelay's platform list, saves,
// restarts the proxy and points OBS at LiveDelay. Tells the user what happened.
void RouteObsThroughLiveDelay(QWidget *parent);
void ShowHelp(QWidget *parent);
void AddOverlayToScene(QWidget *parent);
void ShowSettings(QWidget *parent);

// Switches the plugin's texts to `language` ("auto" = OBS's language) and
// tells the dock to redraw itself through OnLanguageChanged.
void ApplyLanguage(const std::string &language);
extern void (*OnLanguageChanged)();

} // namespace livedelay
