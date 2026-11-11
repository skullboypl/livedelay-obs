#include "settings.hpp"
#include "contrast.hpp"
#include "serverpick.hpp"
#include "hotkeys.hpp"
#include "update.hpp"
#include "version.hpp"
#include "dock.hpp"
#include "encode.hpp"
#include "i18n.hpp"
#include "services.hpp"
#include "limits.hpp"
#include "log.hpp"
#include "panels.hpp"
#include "outputs.hpp"
#include "client.hpp"
#include "wizard.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
void ld_set_host(const char *name);
int ld_start(const char *config_path);
size_t ld_last_error(char *buf, size_t len);
}

namespace livedelay {

namespace {

std::string Trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	size_t b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool Yes(std::string v)
{
	for (char &c : v)
		c = char(tolower(static_cast<unsigned char>(c)));
	return v == "yes" || v == "tak" || v == "true" || v == "1" || v == "on";
}

// Empty keys and a repeated ldN (two platforms saved as ld2) take the next free one.
void AssignIngestKeys(std::vector<Destination> &dests)
{
	std::vector<std::string> used;
	for (Destination &d : dests) {
		const bool clash = d.ingest_key.empty() ||
				   std::find(used.begin(), used.end(), d.ingest_key) != used.end();
		if (clash) {
			for (int n = 0;; ++n) {
				std::string key = "ld" + std::to_string(n);
				if (std::find(used.begin(), used.end(), key) == used.end()) {
					d.ingest_key = std::move(key);
					break;
				}
			}
		}
		used.push_back(d.ingest_key);
	}
}

std::string NextIngestKey(const std::vector<Destination> &dests)
{
	for (int n = 0;; ++n) {
		std::string key = "ld" + std::to_string(n);
		bool taken = false;
		for (const Destination &d : dests)
			if (d.ingest_key == key)
				taken = true;
		if (!taken)
			return key;
	}
}

QString Q(const std::string &s)
{
	return QString::fromStdString(s);
}

std::string S(const QString &s)
{
	return s.trimmed().toStdString();
}

} // namespace

Settings Settings::Load(const std::string &path)
{
	Settings s;
	std::ifstream in(path, std::ios::binary);
	std::string line;
	// Which section the next key belongs to. Before 1.0.43 only [dest] was
	// known, and any other header left the previous platform open — so
	// `key = ld1` under [input Pionowe] became that platform's stream key.
	enum class In { Global, Dest, Input, Foreign } at = In::Global;
	while (std::getline(in, line)) {
		line = Trim(line);
		if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF)
			line = line.substr(3); // BOM
		if (line.empty())
			continue;
		if (line.front() == '[' && line.back() == ']') {
			std::string sec = Trim(line.substr(1, line.size() - 2));
			if (sec.rfind("dest ", 0) == 0 || sec == "dest") {
				Destination d;
				d.name = Trim(sec.substr(4));
				s.dests.push_back(d);
				at = In::Dest;
			} else if (sec.rfind("input ", 0) == 0 && !Trim(sec.substr(6)).empty() &&
				   std::none_of(s.inputs.begin(), s.inputs.end(),
						[&](const Input &x) { return x.name == Trim(sec.substr(6)); })) {
				// A repeated name (the engine does the same) stays as foreign text.
				Input i;
				i.name = Trim(sec.substr(6));
				s.inputs.push_back(i);
				at = In::Input;
			} else {
				s.foreign.push_back(line);
				at = In::Foreign;
			}
			continue;
		}
		// A section we do not know is copied word for word, comments included.
		if (at == In::Foreign) {
			s.foreign.push_back(line);
			continue;
		}
		if (line[0] == '#' || line[0] == ';')
			continue;
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
		if (at == In::Input) {
			Input &i = s.inputs.back();
			if (k == "key")
				i.key = v;
			else if (k == "feed")
				i.external = v == "external";
			else if (k == "own")
				i.own = Yes(v);
			else
				i.extra.push_back(line);
			continue;
		}
		if (at == In::Dest) {
			Destination *dest = &s.dests.back();
			if (k == "input")
				dest->input = v;
			else if (k == "url")
				dest->url = v;
			else if (k == "server")
				dest->server_pick = v;
			else if (k == "service")
				dest->service = v;
			else if (k == "channel")
				dest->channel = v;
			else if (k == "key")
				dest->key = v;
			else if (k == "enabled")
				dest->enabled = Yes(v);
			else if (k == "autostart")
				dest->autostart = Yes(v);
			else if (k == "ingest_key")
				dest->ingest_key = v;
			else if (k == "encode_mode")
				dest->encode_mode = ParseEncodeMode(v);
			else if (k == "video_kbps")
				dest->video_kbps = uint32_t(std::atoi(v.c_str()));
			else if (k == "audio_kbps")
				dest->audio_kbps = uint32_t(std::atoi(v.c_str()));
			else if (k == "keyint_sec")
				dest->keyint_sec = uint32_t(std::atoi(v.c_str()));
			else if (k == "rate_control")
				dest->rate_control = v;
			else if (k == "width")
				dest->width = uint32_t(std::atoi(v.c_str()));
			else if (k == "height")
				dest->height = uint32_t(std::atoi(v.c_str()));
			else if (k == "audio_track") {
				const int n = std::atoi(v.c_str());
				dest->audio_track = (n >= 1 && n <= 6) ? n : 0;
			} else if (k == "vod_track") {
				const int n = std::atoi(v.c_str());
				dest->vod_track = (n >= 1 && n <= 6) ? n : 0;
				dest->vod_track_set = true;
			} else if (k == "vod_audio") {
				dest->vod_audio = Yes(v);
				if (dest->vod_audio && !dest->vod_track_set)
					dest->vod_track = -1;
			} else if (k == "vod_override")
				dest->vod_override = Yes(v);
			else if (k == "enhanced_broadcasting" || k == "enhanced")
				dest->enhanced_broadcasting = Yes(v);
			else
				dest->extra.push_back(line);
			continue;
		}
		if (k == "listen")
			s.listen = v;
		else if (k == "api")
			s.api = v;
		else if (k == "delay")
			s.delay = std::atoi(v.c_str());
		else if (k == "max_buffer_mb")
			s.max_buffer_mb = std::atoi(v.c_str());
		else if (k == "language")
			s.language = v.empty() ? "auto" : v;
		else if (k == "dock_theme")
			s.dock_theme = DockThemeKey(ParseDockTheme(v));
		else if (k == "buffer_codec") {
			if (v == "h264" || v == "hevc" || v == "av1" || v == "obs")
				s.buffer_codec = v;
		} else if (k == "single_pc")
			s.single_pc = Yes(v);
		else if (k == "obs_live")
			s.obs_live = Yes(v);
		else if (k == "limits_ack")
			s.limits_ack = v;
		else if (k == "hotkey_arm")
			s.hotkey_arm = v;
		else if (k == "hotkey_activate")
			s.hotkey_activate = v;
		else if (k == "hotkey_cut")
			s.hotkey_cut = v;
		else if (k == "hotkey_cut_after")
			s.hotkey_cut_after = v;
		else if (k.rfind("arm_slot_", 0) == 0) {
			const int slot = std::atoi(k.c_str() + 9);
			int sec = std::atoi(v.c_str());
			if (slot >= 0 && slot < 6) {
				if (sec < 1)
					sec = 1;
				if (sec > 3600)
					sec = 3600;
				s.arm_slot[slot] = sec;
			}
		}
		else if (k == "overlay_live")
			s.overlay_live = v;
		else if (k == "overlay_arming")
			s.overlay_arming = v;
		else if (k == "overlay_armed")
			s.overlay_armed = v;
		else if (k == "overlay_delayed")
			s.overlay_delayed = v;
		else if (k == "delay_mode")
			s.delay_mode = v == "per_input" ? "per_input" : "together";
		else
			s.extra.push_back(line);
	}
	if (s.delay < 1)
		s.delay = 15;
	if (s.max_buffer_mb < 16)
		s.max_buffer_mb = 512;
	if (s.inputs.empty()) {
		AssignIngestKeys(s.dests);
		return s;
	}
	// Same rule as the engine: an input without a key takes ld{i} by order.
	for (size_t i = 0; i < s.inputs.size(); ++i)
		if (s.inputs[i].key.empty())
			s.inputs[i].key = "ld" + std::to_string(i);
	// A platform publishes on its input's key, so platforms sharing an input
	// share an encode. A platform naming no input, or one that is not there,
	// takes the first — the same choice the engine makes, so the plugin
	// encodes onto the key the engine listens for.
	for (Destination &d : s.dests) {
		const Input *i = s.FindInput(d.input);
		if (!i) {
			i = &s.inputs.front();
			d.input = i->name;
		}
		d.ingest_key = i->key;
	}
	// A file the app or the web panel wrote carries no mark: an input feeding
	// exactly one platform of its own name is that platform's own encode.
	for (Input &i : s.inputs) {
		if (i.own || i.external)
			continue;
		int fed = 0;
		bool same = false;
		for (const Destination &d : s.dests)
			if (d.input == i.name) {
				++fed;
				same = d.name == i.name;
			}
		i.own = fed == 1 && same;
	}
	return s;
}

const Input *Settings::FindInput(const std::string &name) const
{
	for (const Input &i : inputs)
		if (i.name == name)
			return &i;
	return nullptr;
}

std::vector<Input> Settings::EffectiveInputs() const
{
	if (!inputs.empty())
		return inputs;
	std::vector<Input> made;
	for (size_t n = 0; n < dests.size(); ++n) {
		Input i;
		i.name = dests[n].name;
		i.key = dests[n].ingest_key.empty() ? "ld" + std::to_string(n) : dests[n].ingest_key;
		i.own = true;
		made.push_back(i);
	}
	return made;
}

std::string FreeIngestKey(const Settings &s)
{
	for (int n = 0;; ++n) {
		const std::string key = "ld" + std::to_string(n);
		bool taken = false;
		for (const Input &i : s.inputs)
			taken = taken || i.key == key;
		for (const Destination &d : s.dests)
			taken = taken || d.ingest_key == key;
		if (!taken)
			return key;
	}
}

bool Settings::Save(const std::string &path) const
{
	std::string text = Text();
	std::string tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out << text;
		if (!out)
			return false;
	}
	return os_rename(tmp.c_str(), path.c_str()) == 0;
}

std::string Settings::Text() const
{
	std::ostringstream o;
	o << "# LiveDelay settings (edited from OBS: Tools -> LiveDelay)\n\n"
	  << "listen = " << listen << "\napi = " << api << "\ndelay = " << delay << "\nmax_buffer_mb = " << max_buffer_mb
	  << "\nlanguage = " << language << "\ndock_theme = " << dock_theme
	  << "\nbuffer_codec = " << (buffer_codec.empty() ? "obs" : buffer_codec)
	  << "\nsingle_pc = " << (single_pc ? "yes" : "no")
	  << "\nobs_live = " << (obs_live ? "yes" : "no") << "\n";
	if (!limits_ack.empty())
		o << "limits_ack = " << limits_ack << "\n";
	// Carried through untouched: these belong to the app, not to this plugin.
	const std::pair<const char *, const std::string *> app_keys[] = {{"hotkey_arm", &hotkey_arm},
									 {"hotkey_activate", &hotkey_activate},
									 {"hotkey_cut", &hotkey_cut},
									 {"hotkey_cut_after", &hotkey_cut_after}};
	for (const auto &kv : app_keys) {
		if (!kv.second->empty())
			o << kv.first << " = " << *kv.second << "\n";
	}
	for (int i = 0; i < 6; ++i)
		o << "arm_slot_" << i << " = " << arm_slot[i] << "\n";
	o << "delay_mode = " << (delay_mode == "per_input" ? "per_input" : "together") << "\n";
	for (const std::string &line : extra)
		o << line << "\n";
	for (const Input &in : inputs) {
		o << "\n[input " << in.name << "]\nkey = " << in.key << "\n";
		if (in.external)
			o << "feed = external\n";
		if (in.own)
			o << "own = yes\n";
		for (const std::string &line : in.extra)
			o << line << "\n";
	}
	for (size_t i = 0; i < dests.size(); ++i) {
		const Destination &d = dests[i];
		const std::string ingest = d.ingest_key.empty() ? ("ld" + std::to_string(i)) : d.ingest_key;
		o << "\n[dest " << d.name << "]\nservice = " << d.service << "\nurl = " << d.url;
		if (!d.channel.empty())
			o << "\nchannel = " << d.channel;
		if (!d.server_pick.empty())
			o << "\nserver = " << d.server_pick;
		o << "\nkey = " << d.key;
		// With inputs the publish key belongs to the input. Writing one on the
		// platform as well would say two different things about the same feed.
		if (inputs.empty())
			o << "\ningest_key = " << ingest;
		else
			o << "\ninput = " << d.input;
		o << "\nencode_mode = " << EncodeModeKey(d.encode_mode)
		  << "\nenabled = " << (d.enabled ? "yes" : "no") << "\nautostart = " << (d.autostart ? "yes" : "no");
		if (d.encode_mode == EncodeMode::Manual) {
			o << "\nvideo_kbps = " << d.video_kbps << "\naudio_kbps = " << d.audio_kbps
			  << "\nkeyint_sec = " << d.keyint_sec << "\nrate_control = " << d.rate_control
			  << "\nwidth = " << d.width << "\nheight = " << d.height;
		}
		const int vod = d.vod_track > 0 ? d.vod_track : 0;
		o << "\naudio_track = " << (d.audio_track >= 1 && d.audio_track <= 6 ? d.audio_track : 0)
		  << "\nvod_track = " << vod
		  << "\nvod_audio = " << (vod > 0 ? "yes" : "no")
		  << "\nvod_override = " << (d.vod_override ? "yes" : "no")
		  << "\nenhanced_broadcasting = " << (d.enhanced_broadcasting ? "yes" : "no") << "\n";
		for (const std::string &line : d.extra)
			o << line << "\n";
	}
	// Sections nobody here owns go back as they came, at the end: order carries
	// no meaning in this file.
	for (const std::string &line : foreign) {
		if (!line.empty() && line.front() == '[')
			o << "\n";
		o << line << "\n";
	}
	return o.str();
}

DockTheme ParseDockTheme(const std::string &key)
{
	if (key == "dark")
		return DockTheme::Dark;
	if (key == "contrast")
		return DockTheme::Contrast;
	return DockTheme::Obs;
}

const char *DockThemeKey(DockTheme theme)
{
	switch (theme) {
	case DockTheme::Dark:
		return "dark";
	case DockTheme::Contrast:
		return "contrast";
	case DockTheme::Obs:
	default:
		return "obs";
	}
}

bool SaveLimitsAck(const std::string &key)
{
	Settings s = Settings::Load(ConfigPath());
	if (s.limits_ack == key)
		return true;
	s.limits_ack = key;
	return s.Save(ConfigPath());
}

DockTheme CurrentDockTheme()
{
	return ParseDockTheme(Settings::Load(ConfigPath()).dock_theme);
}

void (*OnLanguageChanged)() = nullptr;
void (*OnConfigChanged)() = nullptr;

void ApplyLanguage(const std::string &language)
{
	const char *obs = obs_get_locale();
	obs_module_set_locale(language == "auto" || language.empty() ? (obs ? obs : "en-US") : language.c_str());
	if (OnLanguageChanged)
		OnLanguageChanged();
}

int Settings::RtmpPort() const
{
	size_t c = listen.rfind(':');
	return c == std::string::npos ? 1935 : std::atoi(listen.c_str() + c + 1);
}

std::string ConfigPath()
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	char *p = obs_module_config_path("livedelay.ini");
	std::string path = p ? p : "livedelay.ini";
	bfree(p);
	return path;
}

std::string LastError()
{
	char buf[512];
	ld_last_error(buf, sizeof(buf));
	return buf;
}

// The RTMP port OBS may still point at after the engine moved off it; 0 if none.
static int g_moved_from = 0;

std::string StartProxy()
{
	// Lets the web panel tell "served from inside OBS" from "served by the app".
	ld_set_host("obs");
	const int before = Settings::Load(ConfigPath()).RtmpPort();
	const int rc = ld_start(ConfigPath().c_str());
	// The engine takes the next free port when another program holds ours and
	// writes it to the ini; everything here reads the ports from there.
	const Settings now = Settings::Load(ConfigPath());
	const size_t c = now.api.rfind(':');
	const int api = c == std::string::npos ? 0 : std::atoi(now.api.c_str() + c + 1);
	g_api_port = uint16_t(api > 0 && api < 65536 ? api : 7799);
	if (now.RtmpPort() != before && g_moved_from == 0)
		g_moved_from = before;
	FollowMovedRtmpPort();
	std::string err = LastError();
	if (rc != 0 && err.empty())
		err = "LiveDelay could not start";
	return err;
}

void FollowMovedRtmpPort()
{
	if (g_moved_from == 0)
		return;
	obs_service_t *svc = obs_frontend_get_streaming_service();
	if (!svc)
		return; // OBS has not loaded its service yet; FINISHED_LOADING calls again.
	const int now = Settings::Load(ConfigPath()).RtmpPort();
	const std::string from = "127.0.0.1:" + std::to_string(g_moved_from);
	obs_data_t *st = obs_service_get_settings(svc);
	std::string server = obs_data_get_string(st, "server");
	const size_t at = server.find(from);
	// Kept until it works: while OBS loads, the service it hands out is not
	// the profile's yet, and FINISHED_LOADING asks again.
	if (at != std::string::npos) {
		g_moved_from = 0;
		server.replace(at, from.size(), "127.0.0.1:" + std::to_string(now));
		obs_data_set_string(st, "server", server.c_str());
		obs_service_update(svc, st);
		obs_frontend_save_streaming_service();
		blog(LOG_INFO, "[livedelay] OBS stream server moved to %s", server.c_str());
	}
	obs_data_release(st);
}

static std::string ApiBase()
{
	Settings s = Settings::Load(ConfigPath());
	size_t c = s.api.rfind(':');
	return "http://127.0.0.1" + (c == std::string::npos ? std::string(":7799") : s.api.substr(c));
}

std::string OverlayUrl()
{
	return ApiBase() + "/overlay";
}

std::string LivePreviewUrl()
{
	const char *locale = obs_get_locale();
	return ApiBase() + "/live-preview?lang=" + std::string(locale ? locale : "en").substr(0, 2);
}

std::string PanelUrl()
{
	return ApiBase() + "/panel";
}

unsigned long long ConfigStamp()
{
#ifndef _WIN32
	return 0;
#else
	WIN32_FILE_ATTRIBUTE_DATA info{};
	const std::wstring path = QString::fromStdString(ConfigPath()).toStdWString();
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
		return 0;
	return (unsigned long long(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
#endif
}

void AddOverlayToScene(QWidget *parent)
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene) {
		obs_source_release(scene_src);
		return;
	}
	obs_data_t *bs = obs_data_create();
	obs_data_set_string(bs, "url", OverlayUrl().c_str());
	obs_data_set_int(bs, "width", 900);
	obs_data_set_int(bs, "height", 140);
	obs_source_t *src = obs_source_create("browser_source", "LiveDelay overlay", bs, nullptr);
	if (src) {
		obs_scene_add(scene, src);
		obs_source_release(src);
		QMessageBox::information(parent, "LiveDelay",
					 T("Settings.OverlayAdded"));
	} else {
		QMessageBox::warning(parent, "LiveDelay",
				     T("Settings.OverlayFailed"));
	}
	obs_data_release(bs);
	obs_source_release(scene_src);
}

namespace {

// Reads the stream target OBS currently uses, so it can become a LiveDelay destination.
bool CurrentObsTarget(Destination &out, bool &already_livedelay, int port)
{
	obs_service_t *svc = obs_frontend_get_streaming_service();
	if (!svc)
		return false;
	const char *url = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const char *key = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	obs_data_t *st = obs_service_get_settings(svc);
	std::string name = obs_data_get_string(st, "service");
	obs_data_release(st);
	out.url = url ? url : "";
	out.key = key ? key : "";
	out.name = name.empty() ? "Platform" : name;
	out.enabled = true;
	out.service = name;
	already_livedelay = out.url.find("127.0.0.1:" + std::to_string(port)) != std::string::npos;
	if (out.url.empty() || out.url == "auto") {
		const Service *svcInfo = FindService(Q(name));
		out.url = svcInfo ? S(svcInfo->servers.front().url) : "";
	}
	return !out.url.empty() && !out.key.empty();
}

// OBS Multitrack / Enhanced Broadcasting needs a Twitch GoLive config URL. Custom
// RTMP (LiveDelay) has none → "Brak adresu URL z konfiguracją…". Turn it off and
// recreate the output handler so MultitrackVideoOutput is destroyed.
void TryResetObsOutputs()
{
	QWidget *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main)
		return;
	if (QMetaObject::invokeMethod(main, "ResetOutputs", Qt::DirectConnection)) {
		blog(LOG_INFO, "[livedelay] ResetOutputs via QMetaObject");
		return;
	}
#ifdef _WIN32
	// MSVC x64: OBSBasic::ResetOutputs() — this in RCX, same as void (*)(void*).
	if (HMODULE mod = GetModuleHandleW(nullptr)) {
		using ResetFn = void (*)(void *);
		if (auto fn = reinterpret_cast<ResetFn>(GetProcAddress(mod, "?ResetOutputs@OBSBasic@@QEAAXXZ"))) {
			fn(main);
			blog(LOG_INFO, "[livedelay] ResetOutputs via GetProcAddress");
			return;
		}
	}
#endif
	blog(LOG_WARNING,
	     "[livedelay] could not ResetOutputs; restart OBS once if Enhanced Broadcasting was on");
}

bool DisableObsStockEnhancedBroadcasting(bool reset_outputs)
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return false;
	const bool was_on = config_get_bool(cfg, "Stream1", "EnableMultitrackVideo");
	if (!was_on && !config_has_user_value(cfg, "Stream1", "EnableMultitrackVideo"))
		return false;
	config_set_bool(cfg, "Stream1", "EnableMultitrackVideo", false);
	config_save_safe(cfg, "tmp", nullptr);
	// ResetOutputs during Start Streaming destroys the output OBS is about to
	// start, and OBS 32 does not export that method to plugins anyway.
	if (reset_outputs)
		TryResetObsOutputs();
	if (was_on)
		blog(LOG_INFO, "[livedelay] disabled OBS Stream1 EnableMultitrackVideo (stock Enhanced Broadcasting)");
	return was_on;
}

bool PointObsAtLiveDelay(int port);

std::string StockPublishKey(const Settings &s)
{
	if (!s.single_pc)
		return "_idle";
	for (size_t i = 0; i < s.dests.size(); ++i) {
		const Destination &d = s.dests[i];
		if (!d.enabled || !d.autostart || d.url.empty() || d.key.empty())
			continue;
		if (const Input *in = s.FindInput(d.input); in && in->external)
			continue;
		if (!d.ingest_key.empty())
			return d.ingest_key;
		return "ld" + std::to_string(i);
	}
	return "_idle";
}

// Changes the key on the service OBS already holds. Replacing the object while
// Start Streaming is in progress is what makes rtmp_output fail with no error.
void UpdateRoutedService(int port, const std::string &key, bool during_start)
{
	obs_service_t *svc = obs_frontend_get_streaming_service();
	if (!svc) {
		if (!during_start)
			PointObsAtLiveDelay(port);
		return;
	}
	const char *url = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	if (!url || std::string(url).find("127.0.0.1:" + std::to_string(port)) == std::string::npos) {
		if (!during_start)
			PointObsAtLiveDelay(port);
		return;
	}
	const char *cur = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	if (cur && key == cur)
		return;
	obs_data_t *st = obs_service_get_settings(svc);
	obs_data_set_string(st, "key", key.c_str());
	obs_service_update(svc, st);
	obs_data_release(st);
	obs_frontend_save_streaming_service();
	blog(LOG_INFO, "[livedelay] stock publish key set to %s on the existing OBS service", key.c_str());
}

bool RoutedTo(int port)
{
	obs_service_t *svc = obs_frontend_get_streaming_service();
	const char *url = svc ? obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : nullptr;
	return url && std::string(url).find("127.0.0.1:" + std::to_string(port)) != std::string::npos;
}

// Returns true when stock Multitrack/EB was on and we turned it off.
bool PointObsAtLiveDelay(int port)
{
	obs_data_t *s = obs_data_create();
	std::string server = "rtmp://127.0.0.1:" + std::to_string(port) + "/live";
	obs_data_set_string(s, "server", server.c_str());
	// _idle is discarded. Single-PC mode publishes on ldN so that one OBS encode
	// is the delay buffer and a second NVENC session is not started.
	const std::string key = StockPublishKey(Settings::Load(ConfigPath()));
	obs_data_set_string(s, "key", key.c_str());
	obs_data_set_bool(s, "use_auth", false);
	obs_service_t *svc = obs_service_create("rtmp_custom", "default_service", s, nullptr);
	obs_frontend_set_streaming_service(svc);
	obs_frontend_save_streaming_service();
	obs_service_release(svc);
	obs_data_release(s);
	// Stock Multitrack/EB cannot negotiate a config URL for Custom → LiveDelay.
	const bool cleared_eb = DisableObsStockEnhancedBroadcasting(true);
	OutputsEnsureCustomServerVodTrack();
	// Do not change OBS → Output bitrates here: mode "obs" reads them as the
	// template for each platform. Stock publish to _idle is discarded by the proxy.
	return cleared_eb;
}

// Links shown in the settings window only, never on stream.
constexpr const char *kSupportUrl = "https://donate.skullmedia.pl";
constexpr const char *kSmoothWizardUrl = "https://smoothwizard.com/?utm_source=livedelay-obs";

QLabel *Hint(const QString &text)
{
	auto *l = new QLabel(Rich(text, true));
	l->setWordWrap(true);
	l->setTextFormat(Qt::RichText);
	l->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeColor(true)));
	return l;
}

QScrollArea *ScrollWrap(QWidget *inner)
{
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setWidget(inner);
	return scroll;
}

// OBS Settings → Output → Audio Track. Simple mode is always track 1.
int ObsStreamTrackNumber()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return 1;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	if (mode && std::string(mode) == "Advanced") {
		const int track = int(config_get_int(cfg, "AdvOut", "TrackIndex"));
		if (track >= 1 && track <= 6)
			return track;
	}
	return 1;
}

// Old "vod_audio = yes" with no vod_track: use OBS's VOD track, else the other of 1 and 2.
int LegacyVodTrack()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (cfg) {
		const char *mode = config_get_string(cfg, "Output", "Mode");
		if (mode && std::string(mode) == "Advanced" && config_get_bool(cfg, "AdvOut", "VodTrackEnabled")) {
			const int vod = int(config_get_int(cfg, "AdvOut", "VodTrackIndex"));
			if (vod >= 1 && vod <= 6)
				return vod;
		}
	}
	return ObsStreamTrackNumber() == 2 ? 1 : 2;
}

bool SessionOutputsBusy()
{
	return obs_frontend_streaming_active() || obs_frontend_recording_active() || obs_frontend_replay_buffer_active() ||
	       obs_frontend_virtualcam_active();
}

void StopSessionOutputs()
{
	if (obs_frontend_streaming_active())
		obs_frontend_streaming_stop();
	if (obs_frontend_recording_active())
		obs_frontend_recording_stop();
	if (obs_frontend_replay_buffer_active())
		obs_frontend_replay_buffer_stop();
	if (obs_frontend_virtualcam_active())
		obs_frontend_stop_virtualcam();
}

// Only the plugin folder or livedelay-obs.dll, and only on an OBS plugin path.
bool SafeUninstallTarget(const QString &path)
{
	const QFileInfo fi(path);
	const QString name = fi.fileName();
	if (name.compare(QStringLiteral("livedelay-obs"), Qt::CaseInsensitive) != 0 &&
	    name.compare(QStringLiteral("livedelay-obs.dll"), Qt::CaseInsensitive) != 0)
		return false;
	const QString native = QDir::toNativeSeparators(fi.absoluteFilePath());
	if (native.size() < 16)
		return false;
	const QString lower = native.toLower();
	return lower.contains(QStringLiteral("\\obs-studio\\")) || lower.contains(QStringLiteral("\\obs-plugins\\")) ||
	       lower.contains(QStringLiteral("\\plugins\\"));
}

void AddUninstallTarget(QStringList *out, const QString &path)
{
	if (!out || path.isEmpty() || !QFileInfo::exists(path) || !SafeUninstallTarget(path))
		return;
	const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
	for (const QString &have : *out) {
		if (have.compare(native, Qt::CaseInsensitive) == 0)
			return;
	}
	out->append(native);
}

QStringList CollectUninstallTargets()
{
	QStringList out;
	obs_module_t *mod = obs_current_module();
	const char *bin = mod ? obs_get_module_binary_path(mod) : nullptr;
	if (bin) {
		const QString dll = QDir::toNativeSeparators(QString::fromUtf8(bin));
		const QString marker = QStringLiteral("\\plugins\\livedelay-obs");
		const int at = dll.indexOf(marker, 0, Qt::CaseInsensitive);
		if (at >= 0)
			AddUninstallTarget(&out, dll.left(at + marker.size()));
		if (dll.endsWith(QStringLiteral("\\obs-plugins\\64bit\\livedelay-obs.dll"), Qt::CaseInsensitive))
			AddUninstallTarget(&out, dll);
	}
	wchar_t exe[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
		QDir root(QString::fromWCharArray(exe));
		if (root.cdUp() && root.cdUp() && root.cdUp()) {
			AddUninstallTarget(&out, root.filePath(QStringLiteral("obs-plugins/64bit/livedelay-obs.dll")));
			AddUninstallTarget(&out, root.filePath(QStringLiteral("data/obs-plugins/livedelay-obs")));
			AddUninstallTarget(&out, root.filePath(QStringLiteral("plugins/livedelay-obs")));
		}
	}
	const QString pd = qEnvironmentVariable("ProgramData");
	if (!pd.isEmpty())
		AddUninstallTarget(&out, QDir(pd).filePath(QStringLiteral("obs-studio/plugins/livedelay-obs")));
	return out;
}

void LaunchUninstallScript(const QString &script, const QStringList &targets)
{
	QStringList args{QStringLiteral("-NoProfile"),
			 QStringLiteral("-STA"),
			 QStringLiteral("-WindowStyle"),
			 QStringLiteral("Hidden"),
			 QStringLiteral("-ExecutionPolicy"),
			 QStringLiteral("Bypass"),
			 QStringLiteral("-File"),
			 script,
			 QStringLiteral("-ObsPid"),
			 QString::number(GetCurrentProcessId()),
			 QStringLiteral("-UiTitle"),
			 T("Danger.ProgressTitle"),
			 QStringLiteral("-UiWait"),
			 T("Danger.ProgressWaiting"),
			 QStringLiteral("-UiRemoving"),
			 T("Danger.ProgressRemoving"),
			 QStringLiteral("-UiDone"),
			 T("Danger.ProgressDone"),
			 QStringLiteral("-UiFail"),
			 T("Danger.ProgressFailed"),
			 QStringLiteral("-UiClose"),
			 T("Danger.ProgressClose"),
			 QStringLiteral("-UiNone"),
			 T("Danger.ProgressNone")};
	for (const QString &t : targets)
		args << QStringLiteral("-Target") << t;
	if (!QProcess::startDetached(QStringLiteral("powershell.exe"), args)) {
		QWidget *host = static_cast<QWidget *>(obs_frontend_get_main_window());
		QMessageBox::warning(host, QStringLiteral("LiveDelay"), T("Danger.UninstallFailed").arg(QStringLiteral("start")));
		return;
	}
	QTimer::singleShot(450, qApp, [] {
		if (QWidget *main = static_cast<QWidget *>(obs_frontend_get_main_window()))
			QMetaObject::invokeMethod(main, "close", Qt::QueuedConnection);
	});
}

// Returns false when the user backed out, so the button can be used again.
bool BeginUninstall(QWidget *parent)
{
	if (QMessageBox::warning(parent, QStringLiteral("LiveDelay"), T("Danger.UninstallConfirm"),
				 QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return false;
	const QStringList targets = CollectUninstallTargets();
	if (targets.isEmpty()) {
		QMessageBox::warning(parent, QStringLiteral("LiveDelay"), T("Danger.NoPath"));
		return false;
	}
	const char *data = obs_get_module_data_path(obs_current_module());
	const QString src = data ? QDir(QString::fromUtf8(data)).filePath(QStringLiteral("Uninstall-LiveDelay.ps1")) : QString();
	if (src.isEmpty() || !QFileInfo::exists(src)) {
		QMessageBox::warning(parent, QStringLiteral("LiveDelay"), T("Danger.ScriptMissing"));
		return false;
	}
	const QString script = QDir::temp().filePath(QStringLiteral("livedelay-uninstall.ps1"));
	QFile::remove(script);
	if (!QFile::copy(src, script)) {
		QMessageBox::warning(parent, QStringLiteral("LiveDelay"), T("Danger.UninstallFailed").arg(QStringLiteral("script")));
		return false;
	}
	auto go = [script, targets] { LaunchUninstallScript(script, targets); };
	if (!SessionOutputsBusy()) {
		go();
		return true;
	}
	StopSessionOutputs();
	auto *wait = new QTimer(qApp);
	auto left = std::make_shared<int>(75);
	QObject::connect(wait, &QTimer::timeout, qApp, [wait, left, go] {
		if (SessionOutputsBusy() && *left > 0) {
			--*left;
			StopSessionOutputs();
			return;
		}
		wait->stop();
		wait->deleteLater();
		go();
	});
	wait->start(200);
	return true;
}

class SettingsDialog : public QDialog {
public:
	explicit SettingsDialog(QWidget *parent) : QDialog(parent), s_(Settings::Load(ConfigPath()))
	{
		setWindowTitle(T("Settings.Title"));
		setMinimumSize(520, 420);
		resize(720, 560);
		if (QScreen *scr = QApplication::primaryScreen()) {
			const QRect ag = scr->availableGeometry();
			resize(qMin(780, int(ag.width() * 0.9)), qMin(640, int(ag.height() * 0.85)));
		}

		delay_ = new QSpinBox;
		delay_->setRange(1, 3600);
		delay_->setSuffix(" s");
		delay_->setValue(s_.delay);
		buffer_ = new QSpinBox;
		buffer_->setRange(16, 16384);
		buffer_->setSuffix(" MB");
		buffer_->setValue(s_.max_buffer_mb);
		buffer_->setToolTip(T("Settings.BufferTip"));
		bufferSuggest_ = new QPushButton;
		bufferSuggest_->setToolTip(T("Settings.BufferTip"));
		bufferHint_ = new QLabel;
		bufferHint_->setWordWrap(true);
		bufferHint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		auto *bufferRow = new QWidget;
		auto *bufferLay = new QVBoxLayout(bufferRow);
		bufferLay->setContentsMargins(0, 0, 0, 0);
		bufferLay->setSpacing(4);
		auto *bufferTop = new QHBoxLayout;
		bufferTop->setContentsMargins(0, 0, 0, 0);
		bufferTop->addWidget(buffer_, 1);
		bufferTop->addWidget(bufferSuggest_);
		bufferLay->addLayout(bufferTop);
		bufferLay->addWidget(bufferHint_);
		language_ = new QComboBox;
		const char *obsLocale = obs_get_locale();
		language_->addItem(T("Settings.LanguageAuto").arg(obsLocale ? obsLocale : "en-US"), "auto");
		language_->addItem("English", "en-US");
		language_->addItem("Polski", "pl-PL");
		language_->addItem("Deutsch", "de-DE");
		language_->addItem(QString::fromUtf8("Русский"), "ru-RU");
		{
			int i = language_->findData(Q(s_.language));
			language_->setCurrentIndex(i < 0 ? 0 : i);
		}
		// The look of this dock only. What viewers see has its own themes in
		// the overlay editor, so it is not repeated here.
		dockTheme_ = new QComboBox;
		dockTheme_->addItem(T("Settings.DockThemeObs"), "obs");
		dockTheme_->addItem(T("Settings.DockThemeDark"), "dark");
		dockTheme_->addItem(T("Settings.DockThemeContrast"), "contrast");
		dockTheme_->setToolTip(T("Settings.DockThemeTip"));
		{
			int i = dockTheme_->findData(Q(s_.dock_theme));
			dockTheme_->setCurrentIndex(i < 0 ? 0 : i);
		}
		// Inputs as the engine runs them. A file without any gets one per
		// platform named after it; each platform points at its own, so the
		// pickers below have something true to show.
		ins_ = s_.EffectiveInputs();
		if (s_.inputs.empty())
			for (size_t n = 0; n < s_.dests.size() && n < ins_.size(); ++n)
				s_.dests[n].input = ins_[n].name;
		original_ = s_;
		auto *general = new QFormLayout;
		general->addRow(T("Settings.DefaultDelay"), delay_);
		general->addRow(T("Settings.BufferLimit"), bufferRow);
		general->addRow(T("Settings.Language"), language_);
		general->addRow(T("Settings.DockTheme"), dockTheme_);
		bufferCodec_ = new QComboBox;
		bufferCodec_->addItem(T("Encode.CodecObs"), "obs");
		bufferCodec_->addItem(T("Encode.CodecH264"), "h264");
		bufferCodec_->addItem(T("Encode.CodecHevc"), "hevc");
		bufferCodec_->addItem(T("Encode.CodecAv1"), "av1");
		bufferCodec_->setToolTip(T("Encode.BufferCodecTip"));
		{
			int i = bufferCodec_->findData(Q(s_.buffer_codec.empty() ? "obs" : s_.buffer_codec));
			bufferCodec_->setCurrentIndex(i < 0 ? 0 : i);
		}
		singlePc_ = new QCheckBox(T("Settings.SinglePc"));
		singlePc_->setToolTip(T("Settings.SinglePcTip"));
		singlePc_->setChecked(s_.single_pc);
		obsLive_ = new QCheckBox(T("Settings.ObsLive"));
		obsLive_->setToolTip(T("Settings.ObsLiveTip"));
		obsLive_->setChecked(s_.obs_live);
		general->addRow(T("Encode.BufferCodec"), bufferCodec_);
		general->addRow(QString(), singlePc_);
		general->addRow(QString(), obsLive_);
		connect(delay_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { RefreshBufferHint(); });
		connect(buffer_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { RefreshBufferHint(); });
		connect(bufferSuggest_, &QPushButton::clicked, this, [this] {
			const int rec = RecommendedBufferMb();
			if (rec > 0)
				buffer_->setValue(rec);
		});

		// Platforms: list on the left, the selected one's fields on the right.
		list_ = new QListWidget;
		list_->setMaximumWidth(170);
		name_ = new QLineEdit;
		service_ = new QComboBox;
		service_->addItem(T("Settings.CustomService"));
		for (const Service &sv : Services())
			service_->addItem(sv.name);
		server_ = new QComboBox;
		keyLink_ = new QLabel;
		keyLink_->setOpenExternalLinks(true);
		url_ = new QLineEdit;
		url_->setPlaceholderText(T("Settings.ServerPlaceholder"));
		key_ = new QLineEdit;
		channel_ = new QLineEdit;
		channel_->setPlaceholderText(T("Settings.ChannelPlaceholder"));
		channel_->setToolTip(T("Settings.ChannelTip"));
		panelsBox_ = new QGroupBox(T("Panels.Title"));
		auto *panelsLay = new QVBoxLayout(panelsBox_);
		panelsAbout_ = new QLabel(T("Panels.About"));
		panelsAbout_->setWordWrap(true);
		panelsAbout_->setOpenExternalLinks(true);
		panelsLay->addWidget(panelsAbout_);
		panelsList_ = new QWidget;
		panelsListLay_ = new QVBoxLayout(panelsList_);
		panelsListLay_->setContentsMargins(0, 0, 0, 0);
		panelsLay->addWidget(panelsList_);
		panelsAdd_ = new QPushButton(T("Panels.Add"));
		panelsNote_ = new QLabel;
		panelsNote_->setWordWrap(true);
		auto *panelsRow = new QHBoxLayout;
		panelsRow->addWidget(panelsAdd_);
		panelsRow->addWidget(panelsNote_, 1);
		panelsLay->addLayout(panelsRow);
		connect(channel_, &QLineEdit::textChanged, this, [this] { RefreshPanels(); });
		connect(panelsAdd_, &QPushButton::clicked, this, [this] { AddPickedPanels(); });
		key_->setEchoMode(QLineEdit::Password);
		auto *show = new QCheckBox(T("Settings.Show"));
		enabled_ = new QCheckBox(T("Settings.SendHere"));
		autostart_ = new QCheckBox(T("Settings.Autostart"));
		autostart_->setToolTip(T("Settings.AutostartTip"));
		encodeMode_ = new QComboBox;
		encodeMode_->addItem(T("Encode.ModeAuto"), int(EncodeMode::Auto));
		encodeMode_->addItem(T("Encode.ModeRecommended"), int(EncodeMode::Recommended));
		encodeMode_->addItem(T("Encode.ModeTwitchPartner"), int(EncodeMode::TwitchPartner));
		encodeMode_->addItem(T("Encode.ModeObs"), int(EncodeMode::Obs));
		encodeMode_->addItem(T("Encode.ModeManual"), int(EncodeMode::Manual));
		encodeMode_->setToolTip(T("Encode.ModeTip"));
		encodeSummary_ = new QLabel;
		encodeSummary_->setWordWrap(true);
		encodeSummary_->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeColor(true)));
		videoKbps_ = new QSpinBox;
		videoKbps_->setRange(100, 100000);
		videoKbps_->setSuffix(" kb/s");
		audioKbps_ = new QSpinBox;
		audioKbps_->setRange(32, 512);
		audioKbps_->setSuffix(" kb/s");
		keyint_ = new QSpinBox;
		keyint_->setRange(0, 10);
		keyint_->setSuffix(" s");
		rateControl_ = new QComboBox;
		rateControl_->addItems({"CBR", "VBR", "CQP", "CRF"});
		width_ = new QSpinBox;
		width_->setRange(0, 7680);
		width_->setSpecialValueText(T("Encode.Canvas"));
		height_ = new QSpinBox;
		height_->setRange(0, 4320);
		height_->setSpecialValueText(T("Encode.Canvas"));
		liveTrack_ = new QComboBox;
		liveTrack_->setToolTip(T("Encode.LiveTrackTip"));
		liveTrack_->addItem(T("Encode.LiveTrackObs"), 0);
		for (int i = 1; i <= 6; ++i)
			liveTrack_->addItem(T("Encode.TrackN").arg(i), i);
		vodTrack_ = new QComboBox;
		vodTrack_->setToolTip(T("Encode.VodTrackTip"));
		vodTrack_->addItem(T("Encode.VodTrackOff"), 0);
		for (int i = 1; i <= 6; ++i)
			vodTrack_->addItem(T("Encode.TrackN").arg(i), i);
		vodShow_ = new QCheckBox(T("Encode.VodShow"));
		vodHelp_ = Hint(T("Encode.VodTrackHelp"));
		vodHelp_->hide();
		enhanced_ = new QCheckBox(T("Encode.Enhanced"));
		enhanced_->setToolTip(T("Encode.EnhancedTip"));
		auto *allObs = new QPushButton(T("Encode.AllObs"));
		allObs->setToolTip(T("Encode.AllObsTip"));
		auto *keyRow = new QHBoxLayout;
		keyRow->addWidget(key_, 1);
		keyRow->addWidget(show);
		input_ = new QComboBox;
		input_->setToolTip(T("Inputs.PlatformInputTip"));
		connect(input_, &QComboBox::activated, this, [this] {
			Store();
			if (inFields_)
				ShowIn();
		});
		auto *form = new QFormLayout;
		form->addRow(T("Settings.Name"), name_);
		form->addRow(T("Inputs.PlatformInput"), input_);
		form->addRow(T("Settings.Service"), service_);
		form->addRow(T("Settings.ServerRegion"), server_);
		form->addRow(T("Settings.Server"), url_);
		form->addRow(T("Settings.Key"), keyRow);
		form->addRow(QString(), keyLink_);
		form->addRow(T("Settings.Channel"), channel_);
		form->addRow(QString(), panelsBox_);
		enabled_->hide(); // superseded by autostart + Start/Stop in the panel
		form->addRow(QString(), autostart_);
		form->addRow(T("Encode.Mode"), encodeMode_);
		form->addRow(QString(), encodeSummary_);
		form->addRow(T("Encode.Video"), videoKbps_);
		form->addRow(T("Encode.Audio"), audioKbps_);
		form->addRow(T("Encode.Keyint"), keyint_);
		form->addRow(T("Encode.RateControl"), rateControl_);
		form->addRow(T("Encode.Width"), width_);
		form->addRow(T("Encode.Height"), height_);
		form->addRow(T("Encode.LiveTrack"), liveTrack_);
		form->addRow(QString(), vodShow_);
		form->addRow(T("Encode.VodTrack"), vodTrack_);
		form->addRow(QString(), vodHelp_);
		form->addRow(QString(), enhanced_);
		form->addRow(QString(), allObs);
		auto *fieldsInner = new QWidget;
		fieldsInner->setLayout(form);
		fields_ = ScrollWrap(fieldsInner);
		auto *add = new QPushButton(T("Settings.Add"));
		auto *remove = new QPushButton(T("Settings.Remove"));
		list_->setMinimumWidth(120);
		list_->setMaximumWidth(220);
		auto *listCol = new QVBoxLayout;
		listCol->addWidget(list_, 1);
		auto *listBtns = new QHBoxLayout;
		listBtns->addWidget(add);
		listBtns->addWidget(remove);
		listCol->addLayout(listBtns);
		auto *listWrap = new QWidget;
		listWrap->setLayout(listCol);
		auto *splitter = new QSplitter(Qt::Horizontal);
		splitter->addWidget(listWrap);
		splitter->addWidget(fields_);
		splitter->setStretchFactor(0, 0);
		splitter->setStretchFactor(1, 1);
		splitter->setSizes({160, 480});
		auto *importBtn = new QPushButton(T("Settings.SwitchObs"));
		auto *livePreview = new QPushButton(T("Settings.LivePreview"));
		livePreview->setToolTip(T("Settings.LivePreviewTip"));
		auto *helpBtn = new QPushButton(T("Dock.Help"));
		auto *destPage = new QWidget;
		auto *destLayout = new QVBoxLayout(destPage);
		destLayout->setContentsMargins(8, 8, 8, 8);
		destLayout->addWidget(Hint(T("Settings.PlatformsAbout")));
		destLayout->addWidget(splitter, 1);
		auto *destBtns = new QHBoxLayout;
		destBtns->addWidget(importBtn, 1);
		destBtns->addWidget(livePreview);
		destLayout->addLayout(destBtns);

		// Overlay for a Browser source.
		ovUrl_ = new QLineEdit(Q(OverlayUrl()));
		auto *urlLine = ovUrl_;
		urlLine->setReadOnly(true);
		// Which picture's overlay this page edits: OBS's own, or one another
		// program feeds (the overlay is drawn into the picture, so each has one).
		ovPick_ = new QComboBox;
		ovOwn_ = new QCheckBox(T("Overlay.Own"));
		ovOwn_->setToolTip(T("Overlay.OwnTip"));
		ovPickBox_ = new QWidget;
		auto *pickLay = new QHBoxLayout(ovPickBox_);
		pickLay->setContentsMargins(0, 0, 0, 0);
		pickLay->addWidget(ovPick_, 1);
		pickLay->addWidget(ovOwn_);
		ovNote_ = Hint(T("Overlay.PerInputNote"));
		auto *copy = new QPushButton(T("Settings.Copy"));
		auto *preview = new QPushButton(T("Settings.Preview"));
		preview->setToolTip(T("Settings.PreviewTip"));
		auto *toScene = new QPushButton(T("Settings.AddToScene"));
		ovToScene_ = toScene;
		auto *editor = new QPushButton(T("Settings.ThemeEditor"));
		editor->setToolTip(T("Settings.ThemeEditorTip"));
		auto *urlRow = new QHBoxLayout;
		urlRow->addWidget(urlLine, 1);
		urlRow->addWidget(copy);
		urlRow->addWidget(preview);
		urlRow->addWidget(toScene);
		// Per state: show it or not, its text, and how it will look.
		overlay_ = QJsonDocument::fromJson(QByteArray::fromStdString(Request("GET", "/overlay/config"))).object();
		const QJsonObject texts = overlay_.value("texts").toObject(), showMap = overlay_.value("show").toObject(),
				  colors = overlay_.value("colors").toObject();
		auto *ovForm = new QFormLayout;
		ovForm_ = ovForm;
		ovForm->addRow(T("Overlay.Picture"), ovPickBox_);
		ovForm->addRow(QString(), ovNote_);
		ovForm->addRow(T("Settings.Address"), urlRow);
		const char *kinds[] = {"live", "arming", "armed", "delayed"};
		const char *labels[] = {"Settings.Live", "Settings.Arming", "Settings.Armed", "Settings.Delayed"};
		for (int i = 0; i < 4; i++) {
			OverlayRow &r = rows_[i];
			r.kind = kinds[i];
			r.color = colors.value(r.kind).toString("#3ddc84");
			r.show = new QCheckBox;
			r.show->setToolTip(T("Settings.ShowState"));
			r.show->setChecked(showMap.value(r.kind).toBool(true));
			r.text = new QLineEdit(texts.value(r.kind).toString());
			r.preview = new QLabel;
			r.preview->setMinimumWidth(150);
			auto *line = new QHBoxLayout;
			line->addWidget(r.show);
			line->addWidget(r.text, 1);
			line->addWidget(r.preview);
			ovForm->addRow(T(labels[i]), line);
			auto update = [this, i] { UpdateOverlayPreview(rows_[i]); };
			connect(r.show, &QCheckBox::toggled, this, update);
			connect(r.text, &QLineEdit::textChanged, this, update);
			UpdateOverlayPreview(r);
		}
		armedAsLive_ = new QCheckBox(T("Settings.ArmedAsLive"));
		armedAsLive_->setToolTip(T("Settings.ArmedAsLiveTip"));
		armedAsLive_->setChecked(overlay_.value("armedAsLive").toBool(false));
		ovForm->addRow(QString(), armedAsLive_);
		ov_[QString()] = OvLook{overlay_, overlay_, true, true};
		connect(ovPick_, &QComboBox::currentIndexChanged, this, [this] {
			const QString next = ovPick_->currentData().toString();
			if (next == ovAt_)
				return;
			ReadOvRows();
			ovAt_ = next;
			ShowOvRows();
		});
		connect(ovOwn_, &QCheckBox::toggled, this, [this] { OvEnable(); });
		auto *editorRow = new QHBoxLayout;
		editorRow->addWidget(Hint(T("Settings.ThemeEditorAbout")), 1);
		editorRow->addWidget(editor);
		ovForm->addRow(QString(), editorRow);
		auto *ovPage = new QWidget;
		auto *ovLayout = new QVBoxLayout(ovPage);
		ovLayout->setContentsMargins(8, 8, 8, 8);
		ovLayout->addWidget(Hint(T("Settings.OverlayAbout")));
		ovLayout->addLayout(ovForm);
		ovLayout->addWidget(Hint(T("Settings.OverlayTips")));
		ovLayout->addStretch();

		auto *generalPage = new QWidget;
		auto *generalOuter = new QVBoxLayout(generalPage);
		generalOuter->setContentsMargins(8, 8, 8, 8);
		auto *generalTop = new QHBoxLayout;
		generalTop->addLayout(general, 1);
		generalTop->addWidget(helpBtn, 0, Qt::AlignTop);
		generalOuter->addLayout(generalTop);
		generalOuter->addStretch();

		auto *support = new QPushButton(T("Settings.SupportButton"));
		auto *wizard = new QPushButton(T("Settings.SmoothWizardButton"));
		auto *site = new QPushButton(T("Settings.WebsiteButton"));
		auto *checkUpdate = new QPushButton(T("Update.Check"));
		auto *downloadUpdate = new QPushButton(T("Update.UpdateNow"));
		downloadUpdate->hide();
		auto *updateStatus = new QLabel;
		updateStatus->setWordWrap(true);
		auto *versionLabel = new QLabel(T("Settings.Version").arg(QStringLiteral(LD_VERSION_STRING)));
		versionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
		auto *aboutPage = new QWidget;
		auto *aboutLayout = new QVBoxLayout(aboutPage);
		aboutLayout->setContentsMargins(8, 8, 8, 8);
		aboutLayout->addWidget(versionLabel);
		aboutLayout->addSpacing(8);
		aboutLayout->addWidget(Hint(T("Update.HowItWorks")));
		auto *updateRow = new QHBoxLayout;
		updateRow->setSpacing(8);
		updateRow->addWidget(checkUpdate, 0, Qt::AlignLeft);
		updateRow->addWidget(downloadUpdate, 0, Qt::AlignLeft);
		updateRow->addStretch();
		aboutLayout->addLayout(updateRow);
		updateStatus->setStyleSheet("color: palette(window-text);");
		aboutLayout->addWidget(updateStatus);
		aboutLayout->addSpacing(12);
		auto *streamDeck = Hint(T("Settings.StreamDeckText"));
		streamDeck->setOpenExternalLinks(true);
		aboutLayout->addWidget(streamDeck);
		aboutLayout->addSpacing(12);
		aboutLayout->addWidget(Hint(T("Settings.WebsiteText")));
		aboutLayout->addWidget(site, 0, Qt::AlignLeft);
		aboutLayout->addSpacing(12);
		aboutLayout->addWidget(Hint(T("Settings.SupportText")));
		aboutLayout->addWidget(support, 0, Qt::AlignLeft);
		aboutLayout->addSpacing(12);
		aboutLayout->addWidget(Hint(T("Settings.SmoothWizardText")));
		aboutLayout->addWidget(wizard, 0, Qt::AlignLeft);
		aboutLayout->addSpacing(18);
		auto *danger = new QGroupBox(T("Danger.Title"));
		auto *dangerLay = new QVBoxLayout(danger);
		dangerLay->addWidget(Hint(T("Danger.About")));
		auto *dangerRow = new QHBoxLayout;
		auto *dangerPick = new QComboBox;
		dangerPick->addItem(T("Danger.Choose"), QString());
		dangerPick->addItem(T("Danger.Uninstall"), QStringLiteral("uninstall"));
		auto *dangerRun = new QPushButton(T("Danger.Run"));
		dangerRun->setEnabled(false);
		dangerRow->addWidget(dangerPick, 1);
		dangerRow->addWidget(dangerRun, 0);
		dangerLay->addLayout(dangerRow);
		aboutLayout->addWidget(danger);
		connect(dangerPick, &QComboBox::currentIndexChanged, dangerRun, [dangerPick, dangerRun] {
			dangerRun->setEnabled(!dangerPick->currentData().toString().isEmpty());
		});
		connect(dangerRun, &QPushButton::clicked, this, [this, dangerPick, dangerRun] {
			if (dangerPick->currentData().toString() != QLatin1String("uninstall"))
				return;
			dangerRun->setEnabled(false);
			if (!BeginUninstall(this))
				dangerRun->setEnabled(true);
		});
		aboutLayout->addStretch();
		auto *updateChecker = new UpdateChecker(this);
		connect(checkUpdate, &QPushButton::clicked, this, [updateChecker, updateStatus, checkUpdate, downloadUpdate] {
			checkUpdate->setEnabled(false);
			downloadUpdate->hide();
			updateStatus->setText(T("Update.Checking"));
			updateChecker->Check(true);
		});
		connect(updateChecker, &UpdateChecker::found, this, [this, updateStatus, checkUpdate, downloadUpdate](UpdateInfo info) {
			checkUpdate->setEnabled(true);
			updateStatus->setText(T("Update.Available").arg(info.version).arg(UpdateChecker::CurrentVersion()));
			downloadUpdate->show();
			downloadUpdate->disconnect();
			QObject::connect(downloadUpdate, &QPushButton::clicked, downloadUpdate, [this, info] {
				UpdateChecker::ApplyUpdate(info, this);
			});
		});
		connect(updateChecker, &UpdateChecker::upToDate, this, [updateStatus, checkUpdate, downloadUpdate] {
			checkUpdate->setEnabled(true);
			downloadUpdate->hide();
			updateStatus->setText(T("Update.UpToDate").arg(UpdateChecker::CurrentVersion()));
		});
		connect(updateChecker, &UpdateChecker::failed, this, [updateStatus, checkUpdate, downloadUpdate](const QString &reason) {
			checkUpdate->setEnabled(true);
			downloadUpdate->hide();
			updateStatus->setText(T("Update.Failed").arg(reason));
		});

		auto *hotkeyPage = MakeHotkeyPage(hotkeyKeys_, hotkeySpins_);
		auto *tabs = new QTabWidget;
		tabs->addTab(ScrollWrap(generalPage), T("Settings.TabGeneral"));
		tabs->addTab(ScrollWrap(hotkeyPage), T("Settings.TabHotkeys"));
		tabs->addTab(BuildInputsPage(), T("Settings.TabInputs"));
		tabs->addTab(destPage, T("Settings.TabPlatforms"));
		connect(tabs, &QTabWidget::currentChanged, this, [this] {
			Store();
			InStore();
			ShowIn();
		});
		tabs->addTab(ScrollWrap(BuildAudioPage()), T("Settings.TabAudio"));
		QWidget *ovTab = ScrollWrap(ovPage);
		tabs->addTab(ovTab, T("Settings.TabOverlay"));
		connect(tabs, &QTabWidget::currentChanged, this, [this, tabs, ovTab](int i) {
			if (tabs->widget(i) == ovTab)
				RefreshOvPick();
		});
		RefreshOvPick();
		tabs->addTab(MakeLogPage(), T("Settings.TabLogs"));
		tabs->addTab(ScrollWrap(aboutPage), T("Settings.TabAbout"));

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
		buttons->button(QDialogButtonBox::Save)->setText(T("Settings.Save"));
		buttons->button(QDialogButtonBox::Cancel)->setText(T("Settings.Cancel"));

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(10, 10, 10, 10);
		layout->addWidget(tabs, 1);
		layout->addWidget(buttons);

		for (const Destination &d : s_.dests)
			list_->addItem(Q(d.name));
		connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { Select(row); });
		connect(show, &QCheckBox::toggled, this,
			[this](bool on) { key_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
		connect(service_, &QComboBox::activated, this, [this](int i) {
			FillServers(i > 0 ? service_->itemText(i) : QString(), QString(), QString());
			if (i > 0 && server_->count() > 0) {
				const QString fill = server_->itemData(0).toString();
				// Kick's row has no address: each channel pastes its own. Don't
				// invent one, and don't wipe a Kick URL they already pasted.
				if (!fill.isEmpty())
					url_->setText(fill);
				else if (!IsKickTarget(QString(), url_->text()))
					url_->setText(QString());
			}
			if (i > 0 && (name_->text().trimmed().isEmpty() || name_->text() == T("Settings.NewPlatform"))) {
				name_->setText(service_->itemText(i).section(" - ", 0, 0));
				if (current_ >= 0)
					list_->item(current_)->setText(name_->text());
			}
			UpdateEncodeFields();
			RefreshPanels();
		});
		connect(server_, &QComboBox::activated, this, [this](int i) {
			const QString fill = server_->itemData(i).toString();
			if (!fill.isEmpty())
				url_->setText(fill);
			UpdateEncodeFields();
		});
		connect(url_, &QLineEdit::textChanged, this, [this] { UpdateEncodeFields(); });
		connect(liveTrack_, &QComboBox::currentIndexChanged, this, [this] { UpdateEncodeFields(); });
		connect(vodTrack_, &QComboBox::currentIndexChanged, this, [this] { UpdateEncodeFields(); });
		connect(vodShow_, &QCheckBox::toggled, this, [this](bool on) {
			if (on && !SupportsSeparateVodTrack(service_->currentText(), url_->text())) {
				if (QMessageBox::question(this, T("Encode.VodShow"), T("Encode.VodShowAsk"),
							  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
					vodShow_->blockSignals(true);
					vodShow_->setChecked(false);
					vodShow_->blockSignals(false);
					return;
				}
			}
			UpdateEncodeFields();
		});
		connect(enhanced_, &QCheckBox::toggled, this, [this] { UpdateEncodeFields(); });
		connect(add, &QPushButton::clicked, this, [this] {
			Store();
			Destination d;
			d.name = S(T("Settings.NewPlatform"));
			d.encode_mode = EncodeMode::Recommended;
			GiveInput(d);
			s_.dests.push_back(d);
			list_->addItem(T("Settings.NewPlatform"));
			list_->setCurrentRow(list_->count() - 1);
			name_->setFocus();
		});
		connect(remove, &QPushButton::clicked, this, [this] {
			int row = list_->currentRow();
			if (row < 0)
				return;
			current_ = -1;
			InStore();
			// Its own input goes with it; a shared one stays for the others.
			{
				const std::string selected =
					inCurrent_ >= 0 && inCurrent_ < int(ins_.size()) ? ins_[inCurrent_].name : "";
				const std::string fed = s_.dests[row].input;
				for (size_t n = 0; n < ins_.size(); ++n)
					if (ins_[n].name == fed && ins_[n].own) {
						ins_.erase(ins_.begin() + n);
						break;
					}
				inCurrent_ = -1;
				for (size_t n = 0; n < ins_.size(); ++n)
					if (!selected.empty() && ins_[n].name == selected)
						inCurrent_ = int(n);
			}
			s_.dests.erase(s_.dests.begin() + row);
			delete list_->takeItem(row);
			Select(list_->currentRow());
			RebuildInList();
			ShowIn();
		});
		connect(name_, &QLineEdit::textEdited, this, [this](const QString &t) {
			if (current_ >= 0)
				list_->item(current_)->setText(t);
		});
		connect(encodeMode_, &QComboBox::currentIndexChanged, this, [this] {
			const bool manual = encodeMode_->currentData().toInt() == int(EncodeMode::Manual);
			if (manual && current_ >= 0 && current_ < int(s_.dests.size())) {
				const uint32_t saved = s_.dests[current_].video_kbps;
				if (saved > 0) {
					videoKbps_->blockSignals(true);
					videoKbps_->setValue(int(saved));
					videoKbps_->blockSignals(false);
				} else {
					s_.dests[current_].video_kbps = uint32_t(videoKbps_->value());
				}
			}
			UpdateEncodeFields();
		});
		connect(videoKbps_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
			if (videoKbps_->signalsBlocked())
				return;
			if (encodeMode_->currentData().toInt() != int(EncodeMode::Manual)) {
				const int keep = videoKbps_->value();
				encodeMode_->blockSignals(true);
				const int idx = encodeMode_->findData(int(EncodeMode::Manual));
				if (idx >= 0)
					encodeMode_->setCurrentIndex(idx);
				encodeMode_->blockSignals(false);
				if (videoKbps_->value() != keep) {
					videoKbps_->blockSignals(true);
					videoKbps_->setValue(keep);
					videoKbps_->blockSignals(false);
				}
				if (current_ >= 0 && current_ < int(s_.dests.size()))
					s_.dests[current_].video_kbps = uint32_t(keep);
			}
			UpdateEncodeFields();
		});
		connect(audioKbps_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { UpdateEncodeFields(); });
		connect(allObs, &QPushButton::clicked, this, [this] {
			Store();
			for (Destination &d : s_.dests)
				d.encode_mode = EncodeMode::Obs;
			Select(current_);
		});
		connect(importBtn, &QPushButton::clicked, this, [this] { Import(); });
		connect(livePreview, &QPushButton::clicked, this,
			[] { QDesktopServices::openUrl(QUrl(Q(LivePreviewUrl()))); });
		connect(helpBtn, &QPushButton::clicked, this, [this] { ShowHelp(this); });
		connect(copy, &QPushButton::clicked, this,
			[urlLine] { QApplication::clipboard()->setText(urlLine->text()); });
		connect(preview, &QPushButton::clicked, this, [this] {
			QDesktopServices::openUrl(QUrl(Q(OverlayUrl()) + QStringLiteral("?preview") + OvQuery(QStringLiteral("&"))));
		});
		connect(toScene, &QPushButton::clicked, this, [this] { AddOverlayToScene(this); });
		connect(editor, &QPushButton::clicked, this, [this] {
			const char *locale = obs_get_locale();
			QDesktopServices::openUrl(QUrl(Q(OverlayUrl().substr(0, OverlayUrl().size() - 8) + "/edit-theme?lang=" +
						      std::string(locale ? locale : "en").substr(0, 2)) +
						    OvQuery(QStringLiteral("&"))));
		});
		connect(support, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kSupportUrl)); });
		connect(wizard, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kSmoothWizardUrl)); });
		connect(site, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl("https://livedelay.vxh.pl/")); });
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [this] {
			openWizard_ = false;
			SaveFromFields();
		});

		FillInputCombo();
		list_->setCurrentRow(s_.dests.empty() ? -1 : 0);
		Select(list_->currentRow());
		RefreshBufferHint();
	}

	void SaveFromFields()
	{
		{
			Store();
			InStore();
			s_.delay_mode = S(delayMode_->currentData().toString()) == "per_input" ? "per_input" : "together";
			// Single PC: OBS encodes once, into one input. Platforms left on
			// encodes of their own would wait for a picture that never comes.
			if (singlePc_->isChecked() && ObsEncodes() > 1 &&
			    QMessageBox::question(this, QStringLiteral("LiveDelay"), T("Inputs.SinglePcAsk"),
						  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::Yes) {
				for (size_t n = 0; n < s_.dests.size(); ++n)
					if (const Input *in = FindIn(s_.dests[n].input); !in || !in->external)
						MoveDest(n, EnsureObsShared());
			}
			// Every platform publishes on the key of what feeds it.
			for (Destination &d : s_.dests) {
				if (!FindIn(d.input)) {
					const Input own = OwnFor(d.name);
					ins_.push_back(own);
					d.input = own.name;
				}
				d.ingest_key = FindIn(d.input)->key;
			}
			// Inputs reach the file only when one of them is shared. A file in
			// which every platform encodes for itself keeps the shape it always
			// had: an ingest_key per platform and no [input …] at all.
			bool shared = false;
			for (const Input &in : ins_)
				shared = shared || !in.own;
			s_.inputs = shared ? ins_ : std::vector<Input>();
			s_.delay = delay_->value();
			s_.max_buffer_mb = buffer_->value();
			SaveOverlay();
			s_.language = S(language_->currentData().toString());
			s_.dock_theme = S(dockTheme_->currentData().toString());
			s_.buffer_codec = S(bufferCodec_->currentData().toString());
			if (s_.buffer_codec != "h264" && s_.buffer_codec != "hevc" && s_.buffer_codec != "av1")
				s_.buffer_codec = "obs";
			s_.single_pc = singlePc_->isChecked();
			s_.obs_live = obsLive_->isChecked();
			for (int i = 0; i < 6 && i < int(hotkeySpins_.size()); ++i)
				s_.arm_slot[i] = hotkeySpins_[i]->value();
			ApplyHotkeyPage(hotkeyKeys_, hotkeySpins_);
			SaveAndRestart();
		}
	}

	/// Set when the user asked for the vertical-stream wizard from here: the
	/// window saves, closes, and ShowSettings opens the wizard after it.
	bool openWizard_ = false;

private:
	// ---- inputs ------------------------------------------------------------
	//
	// What the window shows is simpler than what the file holds. There is
	// always "OBS": the picture of this OBS. Behind it sit the platforms' own
	// inputs (each encoding for itself, as every older file has it) and at
	// most one shared input for the platforms that share an encode. Every
	// other input is one another program publishes to — Aitum Vertical, say —
	// and those are the remaining rows of the list.

	QWidget *BuildInputsPage()
	{
		delayMode_ = new QComboBox;
		delayMode_->addItem(T("Inputs.DelayTogether"), "together");
		delayMode_->addItem(T("Inputs.DelayPerInput"), "per_input");
		delayMode_->setCurrentIndex(s_.PerInputDelay() ? 1 : 0);
		delayMode_->setToolTip(T("Inputs.DelayModeTip"));

		inList_ = new QListWidget;
		inList_->setMinimumWidth(120);
		inList_->setMaximumWidth(220);
		auto *add = new QPushButton(T("Settings.Add"));
		add->setToolTip(T("Inputs.AddTip"));
		inRemove_ = new QPushButton(T("Settings.Remove"));
		auto *listCol = new QVBoxLayout;
		listCol->setContentsMargins(0, 0, 0, 0);
		listCol->addWidget(inList_, 1);
		auto *listBtns = new QHBoxLayout;
		listBtns->addWidget(add);
		listBtns->addWidget(inRemove_);
		listCol->addLayout(listBtns);
		auto *listWrap = new QWidget;
		listWrap->setLayout(listCol);

		// The OBS row: nothing to name or paste, only who shares an encode.
		obsPanel_ = new QWidget;
		auto *obsLay = new QVBoxLayout(obsPanel_);
		obsLay->setContentsMargins(0, 0, 0, 0);
		obsLay->addWidget(Hint(T("Inputs.ObsAbout")));
		const QString address = QStringLiteral("rtmp://127.0.0.1:%1/live").arg(s_.RtmpPort());
		{
			auto *obsAddr = new QLineEdit(address);
			obsAddr->setReadOnly(true);
			auto *obsCopy = new QPushButton(T("Settings.Copy"));
			connect(obsCopy, &QPushButton::clicked, this, [address] { QApplication::clipboard()->setText(address); });
			auto *row = new QHBoxLayout;
			row->addWidget(obsAddr, 1);
			row->addWidget(obsCopy);
			auto *addrForm = new QFormLayout;
			addrForm->setContentsMargins(0, 0, 0, 0);
			addrForm->addRow(T("Inputs.Address"), row);
			// The key OBS's own stream lands on. Any other key lands here too,
			// so OBS works without one; only the other inputs need theirs.
			obsKey_ = new QLineEdit;
			obsKey_->setReadOnly(true);
			auto *obsKeyCopy = new QPushButton(T("Settings.Copy"));
			connect(obsKeyCopy, &QPushButton::clicked, this,
				[this] { QApplication::clipboard()->setText(obsKey_->text()); });
			auto *keyRow = new QHBoxLayout;
			keyRow->addWidget(obsKey_, 1);
			keyRow->addWidget(obsKeyCopy);
			addrForm->addRow(T("Inputs.Key"), keyRow);
			obsLay->addLayout(addrForm);
			obsLay->addWidget(Hint(T("Inputs.ObsKeyNote")));
		}
		auto *sharedTitle = new QLabel(T("Inputs.ObsShared"));
		sharedTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
		obsLay->addWidget(sharedTitle);
		obsBoxes_ = new QWidget;
		obsBoxesLay_ = new QVBoxLayout(obsBoxes_);
		obsBoxesLay_->setContentsMargins(0, 0, 0, 0);
		obsBoxesLay_->setSpacing(4);
		obsLay->addWidget(obsBoxes_);
		obsSingle_ = new QLabel(T("Inputs.SinglePcNote"));
		obsSingle_->setWordWrap(true);
		obsSingle_->setStyleSheet(QStringLiteral("color: #ffbf3c;"));
		obsLay->addWidget(obsSingle_);
		obsLay->addWidget(Hint(T("Inputs.ObsSharedNote")));

		// An input another program feeds.
		inName_ = new QLineEdit;
		inKey_ = new QLineEdit;
		inKey_->setReadOnly(true);
		inKey_->setToolTip(T("Inputs.KeyTip"));
		auto *copyKey = new QPushButton(T("Settings.Copy"));
		auto *keyRow = new QHBoxLayout;
		keyRow->addWidget(inKey_, 1);
		keyRow->addWidget(copyKey);
		auto *addr = new QLineEdit(address);
		addr->setReadOnly(true);
		auto *copyAddr = new QPushButton(T("Settings.Copy"));
		auto *addrRow = new QHBoxLayout;
		addrRow->addWidget(addr, 1);
		addrRow->addWidget(copyAddr);
		// The overlay is drawn into the picture, so this picture takes its own.
		inOverlay_ = new QLineEdit;
		inOverlay_->setReadOnly(true);
		auto *copyOverlay = new QPushButton(T("Settings.Copy"));
		auto *overlayLook = new QPushButton(T("Inputs.OverlayLook"));
		overlayLook->setToolTip(T("Inputs.OverlayLookTip"));
		auto *overlayRow = new QHBoxLayout;
		overlayRow->addWidget(inOverlay_, 1);
		overlayRow->addWidget(copyOverlay);
		overlayRow->addWidget(overlayLook);
		inPlatforms_ = new QWidget;
		inPlatformsLay_ = new QVBoxLayout(inPlatforms_);
		inPlatformsLay_->setContentsMargins(0, 0, 0, 0);
		inPlatformsLay_->setSpacing(4);
		auto *form = new QFormLayout;
		form->setContentsMargins(0, 0, 0, 0);
		form->addRow(Hint(T("Inputs.ExternalAbout")));
		form->addRow(T("Settings.Name"), inName_);
		form->addRow(T("Inputs.Address"), addrRow);
		form->addRow(T("Inputs.Key"), keyRow);
		form->addRow(T("Inputs.Platforms"), inPlatforms_);
		form->addRow(T("Inputs.Overlay"), overlayRow);
		form->addRow(QString(), Hint(T("Inputs.OverlayAbout")));
		inFields_ = new QWidget;
		inFields_->setLayout(form);

		auto *right = new QWidget;
		auto *rightLay = new QVBoxLayout(right);
		rightLay->setContentsMargins(8, 0, 0, 0);
		rightLay->addWidget(obsPanel_);
		rightLay->addWidget(inFields_);
		rightLay->addStretch();

		auto *splitter = new QSplitter(Qt::Horizontal);
		splitter->addWidget(listWrap);
		splitter->addWidget(right);
		splitter->setStretchFactor(0, 0);
		splitter->setStretchFactor(1, 1);
		splitter->setSizes({160, 480});

		auto *wizard = new QPushButton(T("Wizard.Open"));
		wizard->setToolTip(T("Wizard.OpenTip"));

		auto *page = new QWidget;
		auto *lay = new QVBoxLayout(page);
		lay->setContentsMargins(8, 8, 8, 8);
		lay->addWidget(Hint(T("Inputs.About")));
		lay->addWidget(splitter, 1);
		auto *modeRow = new QFormLayout;
		modeRow->addRow(T("Inputs.DelayMode"), delayMode_);
		lay->addLayout(modeRow);
		lay->addWidget(Hint(T("Inputs.BufferNote")));
		lay->addWidget(wizard, 0, Qt::AlignLeft);

		connect(inList_, &QListWidget::currentRowChanged, this, [this](int row) {
			InStore();
			const int at = row < 0 ? kObsRow : inList_->item(row)->data(Qt::UserRole).toInt();
			inCurrent_ = at == kObsRow ? -1 : at;
			ShowIn();
		});
		connect(inName_, &QLineEdit::textEdited, this, [this](const QString &t) {
			if (QListWidgetItem *item = inList_->currentItem(); item && inCurrent_ >= 0)
				item->setText(t);
			inOverlay_->setText(InputOverlayUrl(t));
		});
		connect(copyOverlay, &QPushButton::clicked, this,
			[this] { QApplication::clipboard()->setText(inOverlay_->text()); });
		connect(overlayLook, &QPushButton::clicked, this, [this] {
			const char *locale = obs_get_locale();
			const QString base = Q(OverlayUrl().substr(0, OverlayUrl().size() - 8));
			QDesktopServices::openUrl(QUrl(base + QStringLiteral("/edit-theme?lang=") +
						       Q(std::string(locale ? locale : "en").substr(0, 2)) +
						       QStringLiteral("&input=") +
						       QString::fromUtf8(QUrl::toPercentEncoding(inName_->text().trimmed()))));
		});
		connect(inName_, &QLineEdit::editingFinished, this, [this] { InStore(); });
		connect(copyKey, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(inKey_->text()); });
		connect(copyAddr, &QPushButton::clicked, this, [address] { QApplication::clipboard()->setText(address); });
		connect(add, &QPushButton::clicked, this, [this] {
			InStore();
			Store();
			// OBS is always there, so whatever is added comes from elsewhere.
			Input in;
			in.external = true;
			in.name = S(T("Wizard.InputName"));
			for (int n = 2; FindIn(in.name); ++n)
				in.name = S(T("Inputs.NewName").arg(n));
			in.key = FreeKeyHere();
			ins_.push_back(in);
			inCurrent_ = int(ins_.size()) - 1;
			RebuildInList();
			ShowIn();
			FillInputCombo();
			inName_->setFocus();
			inName_->selectAll();
		});
		connect(inRemove_, &QPushButton::clicked, this, [this] {
			if (inCurrent_ < 0 || inCurrent_ >= int(ins_.size()))
				return;
			Store();
			const std::string gone = ins_[inCurrent_].name;
			// Its platforms go back to the picture of OBS, encoding for themselves.
			for (size_t n = 0; n < s_.dests.size(); ++n)
				if (s_.dests[n].input == gone)
					MoveDest(n, std::string());
			for (size_t n = 0; n < ins_.size(); ++n)
				if (ins_[n].name == gone) {
					ins_.erase(ins_.begin() + n);
					break;
				}
			inCurrent_ = -1;
			RebuildInList();
			ShowIn();
			FillInputCombo();
		});
		connect(wizard, &QPushButton::clicked, this, [this] {
			openWizard_ = true;
			SaveFromFields();
		});

		RebuildInList();
		ShowIn();
		return ScrollWrap(page);
	}

	Input *FindIn(const std::string &name)
	{
		for (Input &in : ins_)
			if (in.name == name)
				return &in;
		return nullptr;
	}

	/// The input shared by the platforms that share OBS's encode, or -1.
	int ObsShared() const
	{
		for (int n = 0; n < int(ins_.size()); ++n)
			if (!ins_[n].own && !ins_[n].external)
				return n;
		return -1;
	}

	/// Its name, making it first if nobody shares an encode yet.
	std::string EnsureObsShared()
	{
		if (const int at = ObsShared(); at >= 0)
			return ins_[at].name;
		Input in;
		in.name = "OBS";
		for (int n = 2; FindIn(in.name); ++n)
			in.name = "OBS " + std::to_string(n);
		in.key = FreeKeyHere();
		ins_.push_back(in);
		return in.name;
	}

	std::string FreeKeyHere() const
	{
		for (int n = 0;; ++n) {
			const std::string key = "ld" + std::to_string(n);
			bool taken = false;
			for (const Input &in : ins_)
				taken = taken || in.key == key;
			if (!taken)
				return key;
		}
	}

	/// A platform's own input: named after it, on a key nobody uses.
	Input OwnFor(const std::string &platform)
	{
		Input own;
		own.own = true;
		own.name = platform;
		for (int n = 2; FindIn(own.name); ++n)
			own.name = platform + " " + std::to_string(n);
		own.key = FreeKeyHere();
		return own;
	}

	/// What feeds a platform that just appeared: OBS, encoded for it alone.
	void GiveInput(Destination &d)
	{
		const Input own = OwnFor(d.name);
		ins_.push_back(own);
		d.input = own.name;
		d.ingest_key = own.key;
		FillInputCombo();
	}

	/// Points platform `n` at the input `target`, or — with an empty name —
	/// back at OBS with an encode of its own. An own input nobody needs any
	/// more goes with the move, and so does the shared one once it is empty.
	/// ins_ may shift, so the selection is found again by name.
	void MoveDest(size_t n, const std::string &target)
	{
		if (n >= s_.dests.size())
			return;
		Destination &d = s_.dests[n];
		const std::string selected = inCurrent_ >= 0 && inCurrent_ < int(ins_.size()) ? ins_[inCurrent_].name : "";
		auto drop = [this](const std::string &name) {
			for (size_t i = 0; i < ins_.size(); ++i)
				if (ins_[i].name == name) {
					ins_.erase(ins_.begin() + i);
					return;
				}
		};
		const Input *cur = FindIn(d.input);
		const bool hadOwn = cur && cur->own;
		if (target.empty()) {
			if (hadOwn)
				return;
			const Input own = OwnFor(d.name);
			ins_.push_back(own);
			d.input = own.name;
		} else {
			if (d.input == target || !FindIn(target))
				return;
			const std::string old = d.input;
			d.input = target;
			if (hadOwn)
				drop(old);
		}
		if (const Input *in = FindIn(d.input))
			d.ingest_key = in->key;
		// A shared encode nobody shares is just an entry in the file.
		if (const int shared = ObsShared(); shared >= 0) {
			bool used = false;
			for (const Destination &e : s_.dests)
				used = used || e.input == ins_[shared].name;
			if (!used)
				drop(std::string(ins_[shared].name));
		}
		inCurrent_ = -1;
		for (size_t i = 0; i < ins_.size(); ++i)
			if (!selected.empty() && ins_[i].name == selected)
				inCurrent_ = int(i);
		RebuildInList();
	}

	/// Row one is OBS. The rest are the inputs other programs feed; each row
	/// carries its place in ins_.
	void RebuildInList()
	{
		if (!inList_)
			return;
		inList_->blockSignals(true);
		inList_->clear();
		auto *obs = new QListWidgetItem(QStringLiteral("OBS"));
		obs->setData(Qt::UserRole, kObsRow);
		inList_->addItem(obs);
		int row = 0;
		const int shared = ObsShared();
		for (int n = 0; n < int(ins_.size()); ++n) {
			if (ins_[n].own || n == shared)
				continue;
			auto *item = new QListWidgetItem(Q(ins_[n].name));
			item->setData(Qt::UserRole, n);
			inList_->addItem(item);
			if (n == inCurrent_)
				row = inList_->count() - 1;
		}
		inList_->setCurrentRow(row);
		inList_->blockSignals(false);
		if (row == 0)
			inCurrent_ = -1;
	}

	static void ClearLayout(QLayout *lay)
	{
		while (QLayoutItem *item = lay->takeAt(0)) {
			delete item->widget();
			delete item;
		}
	}

	/// Fills the right-hand side for the row selected in the list.
	void ShowIn()
	{
		const bool other = inCurrent_ >= 0 && inCurrent_ < int(ins_.size());
		obsPanel_->setVisible(!other);
		inFields_->setVisible(other);
		inRemove_->setEnabled(other);
		ClearLayout(obsBoxesLay_);
		ClearLayout(inPlatformsLay_);
		obsKeys_.clear();
		inShown_.clear();
		if (s_.dests.empty())
			(other ? inPlatformsLay_ : obsBoxesLay_)->addWidget(Hint(T("Dock.NoPlatforms")));
		if (!other) {
			const int shared = ObsShared();
			for (size_t n = 0; n < s_.dests.size(); ++n) {
				const Destination &d = s_.dests[n];
				auto *box = new QCheckBox(Q(d.name));
				box->setChecked(shared >= 0 && d.input == ins_[shared].name);
				if (const Input *from = FindIn(d.input); from && from->external)
					box->setToolTip(T("Inputs.TakesFrom").arg(Q(from->name)));
				connect(box, &QCheckBox::toggled, this, [this, n](bool on) {
					Store();
					MoveDest(n, on ? EnsureObsShared() : std::string());
					FillInputCombo();
					RefreshObsKeys();
				});
				auto *key = new QLabel;
				key->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeColor(true)));
				key->setTextInteractionFlags(Qt::TextSelectableByMouse);
				obsKeys_.push_back(key);
				auto *line = new QWidget;
				auto *lineLay = new QHBoxLayout(line);
				lineLay->setContentsMargins(0, 0, 0, 0);
				lineLay->addWidget(box);
				lineLay->addStretch();
				lineLay->addWidget(key);
				obsBoxesLay_->addWidget(line);
			}
			RefreshObsKeys();
			return;
		}
		const Input &in = ins_[inCurrent_];
		inShown_ = in.name;
		inName_->setText(Q(in.name));
		inKey_->setText(Q(in.key));
		inOverlay_->setText(InputOverlayUrl(Q(in.name)));
		for (size_t n = 0; n < s_.dests.size(); ++n) {
			const Destination &d = s_.dests[n];
			auto *box = new QCheckBox(Q(d.name));
			box->setChecked(d.input == in.name);
			connect(box, &QCheckBox::toggled, this, [this, n](bool on) {
				if (inCurrent_ < 0 || inCurrent_ >= int(ins_.size()))
					return;
				Store();
				InStore();
				MoveDest(n, on ? std::string(ins_[inCurrent_].name) : std::string());
				FillInputCombo();
			});
			inPlatformsLay_->addWidget(box);
		}
	}

	/// Beside each platform on the OBS row: the key its picture is published
	/// on. Every encode has its own, and the shared one is one for all.
	/// How many separate encodes of OBS's picture the platforms ask for.
	int ObsEncodes() const
	{
		std::vector<std::string> keys;
		for (const Destination &d : s_.dests)
			for (const Input &in : ins_)
				if (in.name == d.input && !in.external &&
				    std::find(keys.begin(), keys.end(), in.key) == keys.end())
					keys.push_back(in.key);
		return int(keys.size());
	}

	void RefreshObsKeys()
	{
		if (obsKey_) {
			std::string key;
			for (const Input &in : ins_)
				if (!in.external) {
					key = in.key;
					break;
				}
			if (key.empty() && !s_.dests.empty())
				key = s_.dests.front().ingest_key;
			obsKey_->setText(Q(key.empty() ? std::string("ld0") : key));
		}
		// Single PC has exactly one encode to give.
		if (obsSingle_)
			obsSingle_->setVisible(singlePc_ && singlePc_->isChecked() && ObsEncodes() > 1);
		const int shared = ObsShared();
		for (size_t n = 0; n < obsKeys_.size() && n < s_.dests.size(); ++n) {
			const Input *in = FindIn(s_.dests[n].input);
			if (!in)
				obsKeys_[n]->clear();
			else if (in->external)
				obsKeys_[n]->setText(T("Inputs.TakesFrom").arg(Q(in->name)));
			else
				obsKeys_[n]->setText(
					T(shared >= 0 && in == &ins_[shared] ? "Inputs.KeyShared" : "Inputs.KeyOwn").arg(Q(in->key)));
		}
	}

	void InStore()
	{
		if (!inFields_ || inCurrent_ < 0 || inCurrent_ >= int(ins_.size()) || ins_[inCurrent_].own)
			return;
		Input &in = ins_[inCurrent_];
		// The fields have to be showing this very input, or its name would be
		// overwritten with another one's.
		if (in.name != inShown_)
			return;
		std::string name = S(inName_->text());
		name.erase(std::remove(name.begin(), name.end(), ']'), name.end());
		if (name.empty() || (name != in.name && FindIn(name))) {
			// Empty or taken: keep the old name rather than make two inputs one.
			inName_->setText(Q(in.name));
			if (QListWidgetItem *item = inList_->currentItem())
				item->setText(Q(in.name));
			name = in.name;
		}
		if (name != in.name) {
			for (Destination &d : s_.dests)
				if (d.input == in.name)
					d.input = name;
			in.name = name;
			inShown_ = name;
			FillInputCombo();
		}
	}

	// Refills the platform's picker and selects what the model says, not what
	// the widget showed: after a rename or a move the old entry may be gone.
	// Callers Store() first, so the model holds whatever was picked.
	void FillInputCombo()
	{
		if (!input_)
			return;
		input_->blockSignals(true);
		input_->clear();
		input_->addItem(T("Inputs.Own"), QString());
		input_->addItem(T("Inputs.Shared"), QString::fromLatin1(kPickShared));
		const int shared = ObsShared();
		for (int n = 0; n < int(ins_.size()); ++n)
			if (!ins_[n].own && n != shared)
				input_->addItem(Q(ins_[n].name), Q(ins_[n].name));
		int at = 0;
		if (current_ >= 0 && current_ < int(s_.dests.size()))
			if (const Input *in = FindIn(s_.dests[current_].input); in && !in->own)
				at = shared >= 0 && in == &ins_[shared] ? 1 : std::max(0, input_->findData(Q(in->name)));
		input_->setCurrentIndex(at);
		input_->blockSignals(false);
	}

	struct OverlayRow {
		QString kind, color;
		QCheckBox *show = nullptr;
		QLineEdit *text = nullptr;
		QLabel *preview = nullptr;
	};

	// A rough stand-in for the overlay pill, with the sample values the preview page uses.
	void UpdateOverlayPreview(OverlayRow &r)
	{
		QString t = r.text->text();
		t.replace("{s}", "25").replace("{buf}", "12");
		const bool on = r.show->isChecked() && !t.trimmed().isEmpty();
		r.text->setEnabled(r.show->isChecked());
		r.preview->setTextFormat(Qt::RichText);
		r.preview->setText(on ? QString("<span style='color:%1'>&#9679;</span>&nbsp;%2").arg(r.color, t.toHtmlEscaped())
				      : T("Settings.StateHidden"));
		r.preview->setStyleSheet(on ? "background: rgba(20,22,29,.9); color: #f3f4f7; border-radius: 10px; padding: 3px 10px; font-weight: 600;"
					    : QString("color: %1; padding: 3px 10px; font-style: italic;").arg(ThemeColor(true)));
	}

	/// One picture's overlay as the server has it and as edited here.
	struct OvLook {
		QJsonObject saved, now;
		bool ownSaved = true, own = true; // false: shows the main look
	};

	/// "?input=NAME" for the picture on screen, or nothing for OBS's own.
	QString OvQuery(const QString &lead = QStringLiteral("?")) const
	{
		if (ovAt_.isEmpty())
			return QString();
		return lead + QStringLiteral("input=") + QString::fromUtf8(QUrl::toPercentEncoding(ovAt_));
	}

	void ReadOvRows()
	{
		auto it = ov_.find(ovAt_);
		if (it == ov_.end())
			return;
		QJsonObject &now = it->second.now;
		QJsonObject texts = now.value("texts").toObject(), show = now.value("show").toObject();
		for (const OverlayRow &r : rows_) {
			texts[r.kind] = r.text->text().trimmed();
			show[r.kind] = r.show->isChecked();
		}
		now["texts"] = texts;
		now["show"] = show;
		now["armedAsLive"] = armedAsLive_ && armedAsLive_->isChecked();
		if (!ovAt_.isEmpty())
			it->second.own = ovOwn_->isChecked();
	}

	void ShowOvRows()
	{
		if (ov_.find(ovAt_) == ov_.end()) {
			QJsonObject got = QJsonDocument::fromJson(QByteArray::fromStdString(
								  Request("GET", "/overlay/config" + S(OvQuery()))))
						  .object();
			const bool own = !got.value("inherited").toBool(false);
			got.remove("inherited");
			ov_[ovAt_] = OvLook{got, got, own, own};
		}
		const OvLook &look = ov_[ovAt_];
		const QJsonObject texts = look.now.value("texts").toObject(), show = look.now.value("show").toObject(),
				  colors = look.now.value("colors").toObject();
		for (OverlayRow &r : rows_) {
			const QSignalBlocker a(r.show), b(r.text);
			r.color = colors.value(r.kind).toString("#3ddc84");
			r.show->setChecked(show.value(r.kind).toBool(true));
			r.text->setText(texts.value(r.kind).toString());
		}
		armedAsLive_->setChecked(look.now.value("armedAsLive").toBool(false));
		{
			const QSignalBlocker c(ovOwn_);
			ovOwn_->setChecked(look.own);
		}
		ovOwn_->setVisible(!ovAt_.isEmpty());
		ovUrl_->setText(Q(OverlayUrl()) + OvQuery());
		// Add to scene puts it in OBS's current scene: right only for OBS's picture.
		ovToScene_->setEnabled(ovAt_.isEmpty());
		ovToScene_->setToolTip(ovAt_.isEmpty() ? QString() : T("Overlay.OtherScene"));
		OvEnable();
	}

	void OvEnable()
	{
		const bool on = ovAt_.isEmpty() || ovOwn_->isChecked();
		for (OverlayRow &r : rows_) {
			r.show->setEnabled(on);
			UpdateOverlayPreview(r);
			if (!on)
				r.text->setEnabled(false);
		}
		armedAsLive_->setEnabled(on);
	}

	/// The pictures to choose from: OBS's own and every input another program
	/// feeds, as the Inputs tab has them right now.
	void RefreshOvPick()
	{
		if (!ovPick_)
			return;
		ReadOvRows();
		std::vector<QString> names;
		for (const Input &in : ins_)
			if (in.external && !in.name.empty())
				names.push_back(Q(in.name));
		if (std::find(names.begin(), names.end(), ovAt_) == names.end())
			ovAt_.clear();
		{
			const QSignalBlocker block(ovPick_);
			ovPick_->clear();
			ovPick_->addItem(T("Overlay.Main"), QString());
			for (const QString &n : names)
				ovPick_->addItem(n, n);
			ovPick_->setCurrentIndex(std::max(0, ovPick_->findData(ovAt_)));
		}
		const bool perInput = delayMode_ && delayMode_->currentData().toString() == QStringLiteral("per_input");
		ovForm_->setRowVisible(ovPickBox_, !names.empty());
		ovForm_->setRowVisible(ovNote_, names.empty() && perInput);
		ShowOvRows();
	}

	// Texts and on/off go straight to the running overlays; no restart needed.
	void SaveOverlay()
	{
		if (overlay_.isEmpty())
			return;
		ReadOvRows();
		for (auto &[name, look] : ov_) {
			const bool changed = look.own != look.ownSaved || (look.own && look.now != look.saved);
			if (!changed)
				continue;
			const QJsonObject body = look.own ? look.now : QJsonObject{{"inherited", true}};
			const std::string path =
				"/overlay/config" +
				(name.isEmpty() ? std::string() : "?input=" + std::string(QUrl::toPercentEncoding(name).constData()));
			int status = 0;
			Request("POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact).toStdString(), &status);
			if (status != 200) {
				QMessageBox::warning(this, "LiveDelay", T("Settings.SaveFailed"));
				continue;
			}
			look.saved = look.now;
			look.ownSaved = look.own;
			if (name.isEmpty())
				overlay_ = look.now;
		}
	}

	// Lists the servers (regions) of `service`. `pick` is the saved row
	// ("auto" or a server name). Auto shares its URL with the closest ingest,
	// so a URL match alone would snap the combo back to that ingest.
	// The browser docks we can offer for the platform on screen. OBS shows them
	// in Docks once it restarts, so nothing here touches the current session.
	void RefreshPanels()
	{
		while (QLayoutItem *item = panelsListLay_->takeAt(0)) {
			delete item->widget();
			delete item;
		}
		panelChecks_.clear();
		panelDefs_ = PanelsForService(service_->currentIndex() > 0 ? service_->currentText() : QString());
		panelsBox_->setVisible(!panelDefs_.isEmpty());
		if (panelDefs_.isEmpty())
			return;
		bool wantsChannel = false;
		for (const BrowserPanel &panel : panelDefs_) {
			const QString url = PanelAddress(panel, channel_->text());
			auto *box = new QCheckBox(T(panel.nameKey));
			box->setEnabled(!url.isEmpty());
			box->setChecked(!url.isEmpty() && panel.onByDefault);
			box->setToolTip(url.isEmpty() ? T("Panels.NeedChannel") : url);
			if (url.isEmpty())
				wantsChannel = true;
			panelChecks_.push_back(box);
			panelsListLay_->addWidget(box);
		}
		panelsNote_->setText(wantsChannel ? T("Panels.NeedChannel") : QString());
	}

	void AddPickedPanels()
	{
		int picked = 0;
		for (int i = 0; i < panelChecks_.size() && i < panelDefs_.size(); ++i) {
			if (!panelChecks_[i]->isChecked() || !panelChecks_[i]->isEnabled())
				continue;
			const QString url = PanelAddress(panelDefs_[i], channel_->text());
			if (url.isEmpty())
				continue;
			QueueBrowserDock(PanelTitle(panelDefs_[i], name_->text()), url);
			++picked;
		}
		panelsNote_->setText(picked == 0 ? T("Panels.NothingPicked")
						 : T("Panels.Queued").arg(QueuedBrowserDockCount()));
	}


	void FillServers(const QString &service, const QString &url, const QString &pick)
	{
		server_->clear();
		const Service *sv = service.isEmpty() ? nullptr : FindService(service);
		server_->setEnabled(sv != nullptr);
		url_->setReadOnly(false);
		keyLink_->setVisible(sv && !sv->key_link.isEmpty());
		if (!sv) {
			server_->addItem(T("Settings.CustomServer"), url);
			server_->setItemData(0, QString("custom"), Qt::UserRole + 1);
			return;
		}
		if (!sv->key_link.isEmpty())
			keyLink_->setText(QString("<a href=\"%1\">%2</a>").arg(sv->key_link, T("Settings.GetKey")));
		std::vector<ServerRow> rows;
		const QString autoName = T("Settings.AutoServer");
		for (const Server &srv : sv->servers) {
			const bool isAuto = srv.name == autoName;
			const int i = server_->count();
			server_->addItem(srv.name, srv.url);
			server_->setItemData(i, isAuto ? QString("auto") : srv.name, Qt::UserRole + 1);
			rows.push_back({isAuto, S(srv.name), S(srv.url)});
		}
		int match = PickServerRow(rows, S(pick), S(url));
		if (match < 0 && !url.isEmpty()) {
			server_->insertItem(0, T("Settings.CustomServer"), url);
			server_->setItemData(0, QString("custom"), Qt::UserRole + 1);
			match = 0;
		}
		server_->setCurrentIndex(match < 0 ? 0 : match);
	}

	void Store()
	{
		if (current_ < 0 || current_ >= int(s_.dests.size()))
			return;
		Destination &d = s_.dests[current_];
		const std::string wasName = d.name;
		d.name = S(name_->text()).empty() ? "Platform" : S(name_->text());
		// Its own input carries its name; keep the two in step.
		if (d.name != wasName) {
			if (Input *own = FindIn(d.input); own && own->own && !FindIn(d.name)) {
				own->name = d.name;
				d.input = d.name;
			}
		}
		if (input_ && input_->count() > 0) {
			const std::string picked = S(input_->currentData().toString());
			const Input *cur = FindIn(d.input);
			const int shared = ObsShared();
			const bool onOwn = !cur || cur->own;
			const bool onShared = cur && shared >= 0 && cur == &ins_[shared];
			if (picked.empty()) {
				if (!onOwn)
					MoveDest(size_t(current_), std::string());
			} else if (picked == kPickShared) {
				if (!onShared)
					MoveDest(size_t(current_), EnsureObsShared());
			} else if (picked != d.input) {
				MoveDest(size_t(current_), picked);
			}
		}
		d.url = S(url_->text());
		d.server_pick = S(server_->currentData(Qt::UserRole + 1).toString());
		if (d.server_pick == "custom")
			d.server_pick.clear();
		d.key = S(key_->text());
		d.channel = S(channel_->text());
		d.service = service_->currentIndex() > 0 ? S(service_->currentText()) : "";
		d.enabled = true;
		d.autostart = autostart_->isChecked();
		d.encode_mode = EncodeMode(encodeMode_->currentData().toInt());
		// Recommended / Auto / OBS show the resolved number in the spin box.
		// Only Manual stores it, so flipping away and back keeps a typed value.
		if (d.encode_mode == EncodeMode::Manual)
			d.video_kbps = uint32_t(videoKbps_->value());
		d.audio_kbps = uint32_t(audioKbps_->value());
		d.keyint_sec = uint32_t(keyint_->value());
		d.rate_control = S(rateControl_->currentText());
		d.width = uint32_t(width_->value());
		d.height = uint32_t(height_->value());
		d.audio_track = liveTrack_->currentData().toInt();
		int vod = vodTrack_->currentData().toInt();
		int live = d.audio_track >= 1 && d.audio_track <= 6 ? d.audio_track : ObsStreamTrackNumber();
		if (vod == live)
			vod = 0;
		d.vod_track = vod;
		d.vod_track_set = true;
		d.vod_audio = vod > 0;
		d.vod_override = vodShow_->isChecked();
		d.enhanced_broadcasting = enhanced_->isChecked();
		if (d.ingest_key.empty() && current_ >= 0)
			d.ingest_key = "ld" + std::to_string(current_);
	}

	void UpdateEncodeFields()
	{
		const bool manual = encodeMode_->currentData().toInt() == int(EncodeMode::Manual);
		videoKbps_->setEnabled(true);
		for (QWidget *w : {static_cast<QWidget *>(audioKbps_), static_cast<QWidget *>(keyint_),
				   static_cast<QWidget *>(rateControl_), static_cast<QWidget *>(width_),
				   static_cast<QWidget *>(height_)})
			w->setEnabled(manual);
		const bool twitch = IsTwitchTarget(service_->currentText(), url_->text());
		const bool vodOk = SupportsSeparateVodTrack(service_->currentText(), url_->text());
		const bool showVod = vodOk || vodShow_->isChecked();
		vodShow_->setVisible(!vodOk);
		vodTrack_->setVisible(showVod);
		if (QWidget *parent = vodTrack_->parentWidget()) {
			if (auto *form = qobject_cast<QFormLayout *>(parent->layout())) {
				if (QWidget *label = form->labelForField(vodTrack_))
					label->setVisible(showVod);
			}
		}
		const int livePick = liveTrack_->currentData().toInt();
		const int liveNow = livePick >= 1 && livePick <= 6 ? livePick : ObsStreamTrackNumber();
		const int vodNow = vodTrack_->currentData().toInt();
		if (showVod && vodNow > 0 && vodNow == liveNow) {
			vodTrack_->blockSignals(true);
			vodTrack_->setCurrentIndex(0);
			vodTrack_->blockSignals(false);
			vodHelp_->setText(Rich(T("Encode.VodSame"), true));
			vodHelp_->setVisible(true);
		} else {
			vodHelp_->setText(Rich(T("Encode.VodTrackHelp"), true));
			vodHelp_->setVisible(vodOk);
		}
		enhanced_->setEnabled(twitch);
		if (!twitch && enhanced_->isChecked()) {
			enhanced_->blockSignals(true);
			enhanced_->setChecked(false);
			enhanced_->blockSignals(false);
		}
		if (current_ < 0 || current_ >= int(s_.dests.size())) {
			encodeSummary_->clear();
			return;
		}
		Destination tmp = s_.dests[current_];
		tmp.encode_mode = EncodeMode(encodeMode_->currentData().toInt());
		tmp.video_kbps = uint32_t(videoKbps_->value());
		tmp.audio_kbps = uint32_t(audioKbps_->value());
		tmp.keyint_sec = uint32_t(keyint_->value());
		tmp.rate_control = S(rateControl_->currentText());
		tmp.width = uint32_t(width_->value());
		tmp.height = uint32_t(height_->value());
		tmp.audio_track = liveTrack_->currentData().toInt();
		tmp.vod_track = vodTrack_->currentData().toInt();
		tmp.vod_audio = tmp.vod_track > 0;
		tmp.vod_override = vodShow_->isChecked();
		tmp.enhanced_broadcasting = enhanced_->isChecked();
		tmp.service = service_->currentIndex() > 0 ? S(service_->currentText()) : tmp.service;
		tmp.url = S(url_->text());
		const EncodeSettings resolved = ResolveEncode(tmp);
		encodeSummary_->setText(resolved.summary);
		// The box shows the bitrate this platform will actually send. Editing it
		// switches to Manual; that path must not overwrite the number just typed.
		if (!manual && videoKbps_->value() != resolved.video_kbps) {
			videoKbps_->blockSignals(true);
			videoKbps_->setValue(std::max(100, resolved.video_kbps));
			videoKbps_->blockSignals(false);
		}
		RefreshBufferHint();
	}

	// RAM for delay seconds at total kb/s: MB ≈ kbps × s / 8000 (matches Settings.BufferTip).
	int EstimateNeededMb(int *out_peak_kbps, int *out_platforms) const
	{
		const int delay_s = delay_ ? delay_->value() : s_.delay;
		int peak = 0;
		int n = 0;
		for (size_t i = 0; i < s_.dests.size(); ++i) {
			Destination d = s_.dests[i];
			if (int(i) == current_ && encodeMode_ && videoKbps_ && audioKbps_) {
				d.encode_mode = EncodeMode(encodeMode_->currentData().toInt());
				d.video_kbps = uint32_t(videoKbps_->value());
				d.audio_kbps = uint32_t(audioKbps_->value());
				d.keyint_sec = uint32_t(keyint_->value());
				d.rate_control = S(rateControl_->currentText());
				d.width = uint32_t(width_->value());
				d.height = uint32_t(height_->value());
				d.audio_track = liveTrack_->currentData().toInt();
				d.vod_track = vodTrack_->currentData().toInt();
				d.vod_audio = d.vod_track > 0;
				d.enhanced_broadcasting = enhanced_->isChecked();
				d.service = service_->currentIndex() > 0 ? S(service_->currentText()) : d.service;
				d.url = S(url_->text());
			}
			const EncodeSettings e = ResolveEncode(d);
			peak = std::max(peak, e.video_kbps + e.audio_kbps);
			++n;
		}
		if (n == 0) {
			const ObsOutput obs = ReadObsOutput();
			peak = (obs.video_kbps > 0 ? obs.video_kbps : 6000) + (obs.audio_kbps > 0 ? obs.audio_kbps : 160);
			n = 1;
		}
		if (out_peak_kbps)
			*out_peak_kbps = peak;
		if (out_platforms)
			*out_platforms = n;
		// Equal split across platforms: total limit must cover n × one full delay buffer.
		return int(std::ceil(double(peak) * double(delay_s) / 8000.0 * double(n)));
	}

	int RecommendedBufferMb() const
	{
		const int need = EstimateNeededMb(nullptr, nullptr);
		// ~25% headroom, then snap up to a friendly step.
		int rec = int(std::ceil(need * 1.25));
		if (rec < 64)
			rec = 64;
		const int steps[] = {64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384};
		for (int s : steps) {
			if (s >= rec)
				return s;
		}
		return 16384;
	}

	void RefreshBufferHint()
	{
		if (!bufferHint_ || !buffer_ || !bufferSuggest_)
			return;
		int peak = 0, n = 0;
		const int need = EstimateNeededMb(&peak, &n);
		const int limit = buffer_->value();
		const int delay_s = delay_->value();
		const int rec = RecommendedBufferMb();
		bufferSuggest_->setText(T("Settings.BufferSuggest").arg(rec));
		QString line;
		if (n > 1)
			line = T("Settings.BufferEstimateMulti").arg(need).arg(n).arg(delay_s).arg(peak);
		else
			line = T("Settings.BufferEstimate").arg(need).arg(delay_s).arg(peak);
		QString status;
		QString color;
		const bool dark = Luminance(QApplication::palette().color(QPalette::Window)) < 0.4;
		if (limit < need) {
			status = T("Settings.BufferLow");
			color = dark ? QStringLiteral("#ff8a80") : QStringLiteral("#a32020");
		} else if (limit < int(std::ceil(need * 1.15))) {
			status = T("Settings.BufferTight");
			color = dark ? QStringLiteral("#ffbf3c") : QStringLiteral("#8a5200");
		} else {
			const double head = need > 0 ? double(limit) / double(need) : 99.0;
			status = T("Settings.BufferOk").arg(head, 0, 'f', 1);
			color = dark ? QStringLiteral("#3ddc84") : QStringLiteral("#0d7a45");
		}
		bufferHint_->setText(line + "\n" + status);
		bufferHint_->setStyleSheet(QString("color: %1; font-size: 11px;").arg(color));
	}

	void Select(int row)
	{
		Store();
		current_ = row;
		fields_->setEnabled(row >= 0);
		const Destination empty;
		const Destination &d = row >= 0 ? s_.dests[row] : empty;
		name_->setText(Q(d.name));
		FillInputCombo();
		url_->setText(Q(d.url));
		QString svc = Q(d.service);
		if (svc.isEmpty())
			if (const Service *found = ServiceForUrl(Q(d.url)))
				svc = found->name;
		int idx = svc.isEmpty() ? 0 : service_->findText(svc);
		service_->setCurrentIndex(idx < 0 ? 0 : idx);
		FillServers(idx > 0 ? svc : QString(), Q(d.url), Q(d.server_pick));
		// Auto's saved address is a snapshot of whichever ingest was closest.
		// Show the current one, so the next save follows "Automatycznie".
		if (server_->currentData(Qt::UserRole + 1).toString() == "auto") {
			const QString resolved = server_->currentData().toString();
			if (!resolved.isEmpty())
				url_->setText(resolved);
		}
		key_->setText(Q(d.key));
		channel_->setText(Q(d.channel));
		RefreshPanels();
		enabled_->setChecked(d.enabled);
		autostart_->setChecked(d.autostart && d.enabled);
		const int modeIdx = encodeMode_->findData(int(d.encode_mode));
		encodeMode_->blockSignals(true);
		encodeMode_->setCurrentIndex(modeIdx < 0 ? encodeMode_->findData(int(EncodeMode::Obs)) : modeIdx);
		encodeMode_->blockSignals(false);
		videoKbps_->blockSignals(true);
		if (d.encode_mode == EncodeMode::Manual && d.video_kbps > 0) {
			videoKbps_->setValue(int(d.video_kbps));
		} else {
			Destination shown = d;
			if (shown.encode_mode == EncodeMode::Manual)
				shown.encode_mode = EncodeMode::Recommended;
			videoKbps_->setValue(std::max(100, ResolveEncode(shown).video_kbps));
		}
		videoKbps_->blockSignals(false);
		audioKbps_->setValue(d.audio_kbps > 0 ? int(d.audio_kbps) : 160);
		keyint_->setValue(int(d.keyint_sec));
		const int rc = rateControl_->findText(Q(d.rate_control.empty() ? "CBR" : d.rate_control));
		rateControl_->setCurrentIndex(rc < 0 ? 0 : rc);
		width_->setValue(int(d.width));
		height_->setValue(int(d.height));
		const int at = d.audio_track >= 1 && d.audio_track <= 6 ? d.audio_track : 0;
		const int atIdx = liveTrack_->findData(at);
		liveTrack_->blockSignals(true);
		liveTrack_->setCurrentIndex(atIdx < 0 ? 0 : atIdx);
		liveTrack_->blockSignals(false);
		int vt = d.vod_track;
		if (vt < 0)
			vt = LegacyVodTrack();
		if (vt < 0 || vt > 6)
			vt = 0;
		const int vtIdx = vodTrack_->findData(vt);
		vodTrack_->blockSignals(true);
		vodTrack_->setCurrentIndex(vtIdx < 0 ? 0 : vtIdx);
		vodTrack_->blockSignals(false);
		vodShow_->blockSignals(true);
		vodShow_->setChecked(d.vod_override);
		vodShow_->blockSignals(false);
		enhanced_->setChecked(d.enhanced_broadcasting);
		UpdateEncodeFields();
	}

	void Import()
	{
		Store();
		Destination d;
		bool already = false;
		if (CurrentObsTarget(d, already, s_.RtmpPort()) && !already) {
			if (d.ingest_key.empty())
				d.ingest_key = NextIngestKey(s_.dests);
			GiveInput(d);
			s_.dests.push_back(d);
			list_->addItem(Q(d.name));
			list_->setCurrentRow(list_->count() - 1);
		} else if (!already) {
			// OBS had no server/key yet: route it anyway and let the user add the platform here.
			PointObsAtLiveDelay(s_.RtmpPort());
			Destination neu;
			neu.name = S(T("Settings.NewPlatform"));
			neu.ingest_key = NextIngestKey(s_.dests);
			GiveInput(neu);
			s_.dests.push_back(neu);
			list_->addItem(T("Settings.NewPlatform"));
			list_->setCurrentRow(list_->count() - 1);
			QMessageBox::information(this, "LiveDelay", T("Settings.RoutedNoTarget"));
			service_->setFocus();
			return;
		}
		PointObsAtLiveDelay(s_.RtmpPort());
		QMessageBox::information(this, "LiveDelay", already ? T("Settings.AlreadyRouted") : T("Settings.Routed"));
	}

	// What parts of the running system a settings save must touch.
	struct SaveImpact {
		bool proxy = false;   // listen / api / dest URL·key·ingest → tear down RTMP
		bool outputs = false; // encode / VOD / EB / dest add-remove → rebuild encoders
		bool hot = false;     // delay / max_buffer_mb → POST /settings, no disconnect
	};

	static bool DestProxySame(const Destination &a, const Destination &b)
	{
		return a.name == b.name && a.url == b.url && a.key == b.key && a.ingest_key == b.ingest_key &&
		       a.enabled == b.enabled && a.autostart == b.autostart && a.service == b.service;
	}

	static bool DestEncodeSame(const Destination &a, const Destination &b)
	{
		return a.encode_mode == b.encode_mode && a.video_kbps == b.video_kbps && a.audio_kbps == b.audio_kbps &&
		       a.keyint_sec == b.keyint_sec && a.rate_control == b.rate_control && a.width == b.width &&
		       a.height == b.height && a.audio_track == b.audio_track && a.vod_track == b.vod_track &&
		       a.vod_audio == b.vod_audio && a.vod_override == b.vod_override &&
		       a.enhanced_broadcasting == b.enhanced_broadcasting;
	}

	SaveImpact ClassifySave() const
	{
		SaveImpact i;
		if (original_.listen != s_.listen || original_.api != s_.api)
			i.proxy = true;
		if (original_.delay != s_.delay || original_.max_buffer_mb != s_.max_buffer_mb)
			i.hot = true;
		if (original_.single_pc != s_.single_pc || original_.buffer_codec != s_.buffer_codec)
			i.outputs = true;
		if (original_.obs_live != s_.obs_live) {
			i.hot = true;
			// A second encoder already holds the first slot. Drop it, or the
			// OBS publish cannot enter the preview.
			if (!s_.single_pc)
				i.outputs = true;
		}
		// What feeds what. Writing an old file's made-up inputs down changes
		// nothing the engine runs, so only a real difference restarts it.
		{
			const std::vector<Input> a = original_.EffectiveInputs(), b = s_.EffectiveInputs();
			bool same = a.size() == b.size();
			for (size_t n = 0; same && n < a.size(); ++n)
				same = a[n].key == b[n].key && a[n].external == b[n].external;
			for (size_t n = 0; same && n < s_.dests.size() && n < original_.dests.size(); ++n)
				same = original_.dests[n].ingest_key == s_.dests[n].ingest_key;
			if (!same) {
				i.proxy = true;
				i.outputs = true;
			}
		}
		if (original_.dests.size() != s_.dests.size()) {
			i.proxy = true;
			i.outputs = true;
		} else {
			for (size_t n = 0; n < s_.dests.size(); ++n) {
				if (!DestProxySame(original_.dests[n], s_.dests[n]))
					i.proxy = true;
				if (!DestEncodeSame(original_.dests[n], s_.dests[n]))
					i.outputs = true;
			}
		}
		return i;
	}

	void SaveAndRestart()
	{
		const SaveImpact impact = ClassifySave();
		const bool languageChanged = original_.language != s_.language;
		const bool disruptive = impact.proxy || impact.outputs;
		if (disruptive && obs_frontend_streaming_active() &&
		    QMessageBox::question(this, "LiveDelay", T("Settings.RestartWarning")) != QMessageBox::Yes)
			return;
		// LiveDelay is an RTMP proxy. An SRT endpoint pasted from a platform's
		// dashboard would be saved happily and then never connect.
		for (const Destination &d : s_.dests) {
			const QString url = Q(d.url);
			if (url.isEmpty() || url.startsWith("rtmp://") || url.startsWith("rtmps://"))
				continue;
			QMessageBox::warning(this, "LiveDelay", T("Settings.NotRtmp").arg(Q(d.name), url));
			return;
		}
		if (!s_.Save(ConfigPath())) {
			QMessageBox::warning(this, "LiveDelay", T("Settings.SaveFailed"));
			return;
		}
		SyncObsEncoderToBufferCodec();
		if (impact.proxy) {
			std::string err = StartProxy();
			if (!err.empty())
				QMessageBox::warning(this, "LiveDelay", Q(err));
			// Fresh proxy has empty dest publishers — rebuild OBS outputs and restart live ones.
			OutputsReloadConfig();
		} else {
			if (impact.hot) {
				const std::string q = "settings?delay=" + std::to_string(s_.delay) +
						      "&max_buffer_mb=" + std::to_string(s_.max_buffer_mb) +
						      "&obs_live=" + (s_.obs_live ? "1" : "0");
				int status = 0;
				Request("POST", "/" + q, &status);
				if (status != 200)
					blog(LOG_WARNING, "[livedelay] hot settings apply failed (HTTP %d)", status);
			}
			if (impact.outputs)
				OutputsReloadConfig();
		}
		if (languageChanged)
			ApplyLanguage(s_.language);
		// The dock watches the ini anyway; this just saves it the wait.
		if (OnConfigChanged)
			OnConfigChanged();
		accept();
	}

	Settings s_;
	Settings original_;
	// Every input, the platforms' own ones included. A file without [input …]
	// arrives as one own input per platform — what the engine runs for it.
	std::vector<Input> ins_;
	int inCurrent_ = -1;   // place in ins_ of the shared input on screen
	std::string inShown_;  // the input the fields were filled from
	QWidget *inPlatforms_ = nullptr;
	QVBoxLayout *inPlatformsLay_ = nullptr;
	QPushButton *inRemove_ = nullptr;
	QWidget *obsPanel_ = nullptr; // the OBS row: who shares its encode
	QWidget *obsBoxes_ = nullptr;
	QVBoxLayout *obsBoxesLay_ = nullptr;
	/// The Browser source address of one input's overlay.
	static QString InputOverlayUrl(const QString &name)
	{
		return Q(OverlayUrl()) + QStringLiteral("?input=") + QString::fromUtf8(QUrl::toPercentEncoding(name.trimmed()));
	}

	std::vector<QLabel *> obsKeys_; // one per platform, beside its tick
	QLabel *obsSingle_ = nullptr;   // shown when Single PC meets several encodes
	QLineEdit *obsKey_ = nullptr;   // the key OBS's own picture lands on
	static constexpr int kObsRow = -2;                       // list row that is OBS itself
	static constexpr const char *kPickShared = "\x01shared"; // picker entry: OBS, shared encode
	QComboBox *input_ = nullptr; // on the platform form
	QListWidget *inList_ = nullptr;
	QLineEdit *inName_ = nullptr;
	QLineEdit *inKey_ = nullptr;
	QLineEdit *inOverlay_ = nullptr;
	QComboBox *delayMode_ = nullptr;
	QWidget *inFields_ = nullptr;
	QJsonObject overlay_;
	OverlayRow rows_[4];
	std::map<QString, OvLook> ov_; // picture ("" = OBS's own) → its look
	QString ovAt_;                 // the picture on screen
	QComboBox *ovPick_ = nullptr;
	QCheckBox *ovOwn_ = nullptr;
	QWidget *ovPickBox_ = nullptr;
	QLabel *ovNote_ = nullptr;
	QLineEdit *ovUrl_ = nullptr;
	QPushButton *ovToScene_ = nullptr;
	QFormLayout *ovForm_ = nullptr;
	QCheckBox *armedAsLive_ = nullptr;
	QComboBox *language_;
	QComboBox *dockTheme_ = nullptr;
	QComboBox *bufferCodec_ = nullptr;
	QCheckBox *singlePc_ = nullptr;
	QCheckBox *obsLive_ = nullptr;
	std::vector<ComboButton *> hotkeyKeys_;
	std::vector<QSpinBox *> hotkeySpins_;
	QSpinBox *delay_ = nullptr;
	QSpinBox *buffer_ = nullptr;
	QLabel *bufferHint_ = nullptr;
	QPushButton *bufferSuggest_ = nullptr;
	int current_ = -1;
	QListWidget *list_;
	QWidget *fields_;
	QLineEdit *name_, *url_, *key_;
	QComboBox *service_;
	QComboBox *server_;
	QLabel *keyLink_;
	QLineEdit *channel_ = nullptr;
	QGroupBox *panelsBox_ = nullptr;
	QWidget *panelsList_ = nullptr;
	QVBoxLayout *panelsListLay_ = nullptr;
	QLabel *panelsAbout_ = nullptr;
	QLabel *panelsNote_ = nullptr;
	QPushButton *panelsAdd_ = nullptr;
	QVector<QCheckBox *> panelChecks_;
	QVector<BrowserPanel> panelDefs_;
	QCheckBox *enabled_;
	QCheckBox *autostart_;
	QComboBox *encodeMode_;
	QLabel *encodeSummary_;
	QSpinBox *videoKbps_, *audioKbps_, *keyint_, *width_, *height_;
	QComboBox *rateControl_;
	QComboBox *liveTrack_ = nullptr;
	QComboBox *vodTrack_ = nullptr;
	QCheckBox *vodShow_ = nullptr;
	QLabel *vodHelp_ = nullptr;
	QCheckBox *enhanced_;
	std::vector<obs_source_t *> audioSources_;

	static bool CollectAudioSource(void *param, obs_source_t *source)
	{
		auto *out = static_cast<std::vector<obs_source_t *> *>(param);
		if (!(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO))
			return true;
		const char *name = obs_source_get_name(source);
		if (!name || !*name)
			return true;
		if (obs_source_get_ref(source))
			out->push_back(source);
		return true;
	}

	QWidget *BuildAudioPage()
	{
		obs_enum_sources(CollectAudioSource, &audioSources_);
		std::sort(audioSources_.begin(), audioSources_.end(), [](obs_source_t *a, obs_source_t *b) {
			const char *na = obs_source_get_name(a);
			const char *nb = obs_source_get_name(b);
			return QString::fromUtf8(na ? na : "").localeAwareCompare(QString::fromUtf8(nb ? nb : "")) < 0;
		});
		auto *page = new QWidget;
		auto *lay = new QVBoxLayout(page);
		lay->addWidget(Hint(T("Audio.About")));
		if (audioSources_.empty()) {
			lay->addWidget(new QLabel(T("Audio.Empty")));
			lay->addStretch(1);
			return page;
		}
		auto *grid = new QGridLayout;
		grid->addWidget(new QLabel(T("Audio.Source")), 0, 0);
		for (int t = 1; t <= 6; ++t) {
			auto *h = new QLabel(QString::number(t));
			h->setAlignment(Qt::AlignCenter);
			grid->addWidget(h, 0, t);
		}
		for (int row = 0; row < int(audioSources_.size()); ++row) {
			obs_source_t *source = audioSources_[size_t(row)];
			const char *raw = obs_source_get_name(source);
			auto *name = new QLabel(QString::fromUtf8(raw ? raw : ""));
			name->setToolTip(name->text());
			grid->addWidget(name, row + 1, 0);
			const uint32_t mix = obs_source_get_audio_mixers(source);
			for (int t = 0; t < 6; ++t) {
				auto *box = new QCheckBox;
				box->setChecked((mix & (1u << t)) != 0);
				auto *cell = new QWidget;
				auto *cellLay = new QHBoxLayout(cell);
				cellLay->setContentsMargins(0, 0, 0, 0);
				cellLay->addStretch(1);
				cellLay->addWidget(box);
				cellLay->addStretch(1);
				grid->addWidget(cell, row + 1, t + 1);
				connect(box, &QCheckBox::toggled, this, [source, t](bool on) {
					uint32_t bits = obs_source_get_audio_mixers(source);
					if (on)
						bits |= (1u << t);
					else
						bits &= ~(1u << t);
					obs_source_set_audio_mixers(source, bits);
				});
			}
		}
		lay->addLayout(grid);
		lay->addStretch(1);
		return page;
	}

public:
	~SettingsDialog() override
	{
		for (obs_source_t *source : audioSources_)
			obs_source_release(source);
		audioSources_.clear();
	}
};

} // namespace

void ShowSettings(QWidget *parent)
{
	bool wizard = false;
	{
		SettingsDialog dlg(parent);
		wizard = dlg.exec() == QDialog::Accepted && dlg.openWizard_;
	}
	// Saved and closed first, so the wizard reads the file this window wrote.
	if (wizard)
		ShowVerticalWizard(parent);
}

bool ObsRoutedToLiveDelay()
{
	return RoutedTo(Settings::Load(ConfigPath()).RtmpPort());
}

void HardenObsForLiveDelay(bool during_start)
{
	FollowMovedRtmpPort();
	const Settings s = Settings::Load(ConfigPath());
	const int port = s.RtmpPort();
	if (!RoutedTo(port))
		return;
	DisableObsStockEnhancedBroadcasting(!during_start);
	OutputsEnsureCustomServerVodTrack();
	// In place. Building a new service inside STREAMING_STARTING is what made
	// rtmp_output fail before OBS ever connected to LiveDelay.
	UpdateRoutedService(port, StockPublishKey(s), during_start);
}

void RouteObsThroughLiveDelay(QWidget *parent)
{
	Settings s = Settings::Load(ConfigPath());
	Destination d;
	bool already = false;
	if (!CurrentObsTarget(d, already, s.RtmpPort()) && !already) {
		// Nothing to move (OBS had no server or key): route OBS and open the platform list.
		const bool cleared_eb = PointObsAtLiveDelay(s.RtmpPort());
		QString msg = T("Settings.RoutedNoTarget");
		if (cleared_eb)
			msg += "\n\n" + T("Settings.EbCleared");
		QMessageBox::information(parent, "LiveDelay", msg);
		ShowSettings(parent);
		return;
	}
	if (!already) {
		bool known = false;
		for (const Destination &x : s.dests)
			known |= x.url == d.url && x.key == d.key;
		if (!known)
			s.dests.push_back(d);
		if (!s.Save(ConfigPath())) {
			QMessageBox::warning(parent, "LiveDelay", T("Settings.SaveFailed"));
			return;
		}
		StartProxy();
	}
	const bool cleared_eb = PointObsAtLiveDelay(s.RtmpPort());
	OutputsReloadConfig();
	const bool fitted = !EncodeProblems().isEmpty() && FitEncodeForObsMode();
	QString msg = already ? T("Settings.AlreadyRouted") : T("Settings.RoutedSaved");
	if (cleared_eb)
		msg += "\n\n" + T("Settings.EbCleared");
	if (fitted)
		msg += "\n\n" + T("Limits.Fitted").arg(ReadObsOutput().video_kbps).arg(2);
	QMessageBox::information(parent, "LiveDelay", msg);
}

void ShowHelp(QWidget *parent)
{
	QDialog dlg(parent);
	dlg.setWindowTitle(T("Help.Title"));
	dlg.setMinimumWidth(560);
	auto *body = new QLabel(Rich(T("Help.Body").arg(Q(OverlayUrl()))));
	body->setWordWrap(true);
	body->setTextFormat(Qt::RichText);
	body->setStyleSheet(QStringLiteral("color: %1;").arg(ThemeColor()));
	body->setTextInteractionFlags(Qt::TextBrowserInteraction);
	body->setOpenExternalLinks(true);
	auto *route = new QPushButton(T("Help.RouteNow"));
	route->setEnabled(!ObsRoutedToLiveDelay());
	auto *settings = new QPushButton(T("Dock.Settings"));
	auto *close = new QPushButton(T("Help.Close"));
	auto *buttons = new QHBoxLayout;
	buttons->addWidget(route);
	buttons->addWidget(settings);
	buttons->addStretch();
	buttons->addWidget(close);
	auto *layout = new QVBoxLayout(&dlg);
	layout->addWidget(body);
	layout->addLayout(buttons);
	QObject::connect(route, &QPushButton::clicked, &dlg, [&dlg, route] {
		RouteObsThroughLiveDelay(&dlg);
		route->setEnabled(!ObsRoutedToLiveDelay());
	});
	QObject::connect(settings, &QPushButton::clicked, &dlg, [&dlg] {
		dlg.accept();
		ShowSettings(dlg.parentWidget());
	});
	QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
	dlg.exec();
}

} // namespace livedelay
