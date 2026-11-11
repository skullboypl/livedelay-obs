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

/// One RTMP feed into LiveDelay. A platform names the input that feeds it, so
/// several platforms can share one picture and one encode. A file without any
/// [input …] runs as it always did: one input per platform, on its ingest_key.
struct Input {
	std::string name;
	std::string key; // RTMP publish key into LiveDelay (ld0, …)
	/// Fed by another program — Aitum Vertical, a second OBS — rather than by
	/// an encoder this plugin starts. The plugin then never publishes on its
	/// key, or two programs would fight over it. `feed = external` in the ini.
	bool external = false;
	/// One platform's own encode: the arrangement every older file has, named
	/// after the platform it feeds. The interfaces show it as that platform's
	/// "own encode", never as an input. `own = yes` in the ini; an unmarked
	/// input that feeds exactly one platform of its own name counts too.
	bool own = false;
	/// Lines of the section this build does not use, written back as they were.
	std::vector<std::string> extra;
};

// livedelay.ini as the settings window sees it. The proxy reads the same file.
struct Destination {
	std::string name, url, key;
	/// "auto", the ingest's display name, or empty. Not the dial address.
	std::string server_pick;
	bool enabled = true;
	bool autostart = true;
	std::string service; // name from OBS's service list, empty = custom server
	/// Channel name on that platform, as it appears in its own URLs. Chat and
	/// dashboard pages are per channel, so the browser panels need it.
	std::string channel;
	std::string ingest_key; // RTMP key into LiveDelay (ld0, …)
	/// Name of the input feeding this platform. With inputs in the file,
	/// Load() also sets ingest_key to that input's key, so the output code
	/// keeps asking one field.
	std::string input;
	/// Lines this build does not know, kept so a save does not erase them.
	std::vector<std::string> extra;
	EncodeMode encode_mode = EncodeMode::Obs;
	uint32_t video_kbps = 0;
	uint32_t audio_kbps = 0;
	uint32_t keyint_sec = 0;
	std::string rate_control = "CBR";
	uint32_t width = 0;
	uint32_t height = 0;
	/// 0 = OBS Settings → Output stream track. 1–6 = that mixer is what viewers hear.
	int audio_track = 0;
	/// 0 = no VOD track. 1–6 = that mixer is a second AAC.
	/// -1 = old ini had vod_audio=yes and no explicit track.
	int vod_track = 0;
	/// True once the ini contained vod_track, so vod_audio=yes does not mean "legacy".
	bool vod_track_set = false;
	/// Kept in sync with vod_track > 0 so older builds still see the flag.
	bool vod_audio = false;
	/// User asked for a VOD track on a platform LiveDelay does not expect to honor it.
	bool vod_override = false;
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
	/// obs | h264 | hevc | av1. "obs" keeps the OBS Output encoder and does not
	/// open a second NVENC codec. Kick may still require H.264, and only then.
	std::string buffer_codec = "obs";
	/// OBS encodes once into the delay buffer. The plugin does not start another
	/// NVENC session, which is the FPS cost on a single gaming PC.
	bool single_pc = false;
	/// The stream OBS is already sending fills the preview and the kb/s meter.
	/// No second encoder. The open `_idle` publish is picked up on the next frame.
	bool obs_live = false;
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
	/// Empty = the file has no [input …]: one per platform, as before.
	std::vector<Input> inputs;
	/// "together" (one set of buttons for everything) or "per_input". The
	/// engine does not care; it tells the dock what to show.
	std::string delay_mode = "together";
	/// Global lines this build does not know, and whole sections it does not
	/// know (header included). Both go back into the file untouched: the app,
	/// the web panel or a newer build may have written them.
	std::vector<std::string> extra;
	std::vector<std::string> foreign;

	static Settings Load(const std::string &path);
	bool Save(const std::string &path) const;
	std::string Text() const; // the ini as written by Save()
	int RtmpPort() const;
	/// The input called `name`, or nullptr.
	const Input *FindInput(const std::string &name) const;
	/// The inputs as the engine runs them: the file's own, or — for a file
	/// without any — one per platform named after it, on its ingest key.
	std::vector<Input> EffectiveInputs() const;
	bool PerInputDelay() const { return delay_mode == "per_input"; }
};

/// The next ld… that no input and no platform publishes on.
std::string FreeIngestKey(const Settings &s);

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
/// `during_start` is OBS_FRONTEND_EVENT_STREAMING_STARTING. Replacing the
/// service object there makes rtmp_output fail before it connects.
void HardenObsForLiveDelay(bool during_start = false);
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
