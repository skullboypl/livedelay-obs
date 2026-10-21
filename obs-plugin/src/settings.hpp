#pragma once

#include <string>
#include <vector>

class QWidget;

namespace livedelay {

// livedelay.ini as the settings window sees it. The proxy reads the same file.
struct Destination {
	std::string name, url, key;
	bool enabled = true;
};

struct Settings {
	std::string listen = "127.0.0.1:1935";
	std::string api = "127.0.0.1:7799";
	int delay = 15;
	int max_buffer_mb = 512;
	std::string overlay_live = "LIVE";
	std::string overlay_arming = "ARMING… {buf}/{s}s";
	std::string overlay_armed = "ARMED {s}s";
	std::string overlay_delayed = "DELAY {s}s";
	std::vector<Destination> dests;

	static Settings Load(const std::string &path);
	bool Save(const std::string &path) const;
	int RtmpPort() const;
};

// Path of the ini inside OBS's plugin config folder (created if missing).
std::string ConfigPath();
// (Re)starts the embedded proxy. Returns an empty string or the error.
std::string StartProxy();

std::string OverlayUrl();
void AddOverlayToScene(QWidget *parent);
void ShowSettings(QWidget *parent);

} // namespace livedelay
