#pragma once

#include <cstdint>
#include <string>
#include <vector>

class QWidget;

namespace livedelay {

enum class EncodeMode { Auto, Recommended, TwitchPartner, Obs, Manual };

/// Look of the LiveDelay dock inside OBS. Nothing to do with the viewer
/// overlay, which has its own themes in the browser editor (/edit-theme).
enum class DockTheme { Obs, Dark, Contrast };

DockTheme ParseDockTheme(const std::string &key);
const char *DockThemeKey(DockTheme theme);
/// The theme saved in livedelay.ini right now.
DockTheme CurrentDockTheme();

// livedelay.ini as the settings window sees it. The proxy reads the same file.
struct Destination {
	std::string name, url, key;
	/// "auto", the ingest's display name, or empty. Not the dial address.
	std::string server_pick;
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
	/// 0 = OBS Settings → Output stream track. 1–6 = that mixer is what viewers hear.
	int audio_track = 0;
	/// 0 = no VOD track. 1–6 = that mixer is Twitch's VOD AAC (output audio index 1).
	/// -1 = old ini had vod_audio=yes and no explicit track.
	int vod_track = 0;
	/// True once the ini contained vod_track, so vod_audio=yes does not mean "legacy".
	bool vod_track_set = false;
	/// Kept in sync with vod_track > 0 so older builds still see the flag.
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
	std::string dock_theme = "obs"; // obs = follow OBS's theme, or dark / contrast
	/// Limits warning the user dismissed, keyed to the settings it was about.
	/// Empty = show it. A different warning brings the banner back.
	std::string limits_ack;
	/// Global hotkeys of the standalone app. This plugin uses OBS's own hotkeys
	/// and never reads these, but it must not drop them when it saves the ini.
	std::string hotkey_arm, hotkey_activate, hotkey_cut, hotkey_cut_after;
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
// The settings panel the engine serves in a browser; same page as the app's.
std::string PanelUrl();
// Last write time of livedelay.ini, 0 when it cannot be read. The dock polls
// this so a save from the web panel or the app shows up without a restart.
unsigned long long ConfigStamp();
// Remembers a dismissed limits warning in the ini. Empty `key` shows it again.
bool SaveLimitsAck(const std::string &key);
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
// Called right after the plugin's own settings window wrote livedelay.ini.
extern void (*OnConfigChanged)();

} // namespace livedelay
