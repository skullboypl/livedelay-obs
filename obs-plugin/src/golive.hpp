#pragma once

#include "settings.hpp"

#include <string>
#include <vector>

namespace livedelay {

struct GoLiveRung {
	std::string type;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fps_num = 0;
	uint32_t fps_den = 0;
	std::string scale;
	std::string settings_json;
};

struct GoLivePlan {
	std::string url;
	std::string key;
	std::vector<GoLiveRung> rungs;
};

// POST Twitch GetClientConfiguration. False means stay on one classic video track.
bool FetchGoLive(const Destination &dest, GoLivePlan &out, std::string &error);

} // namespace livedelay
