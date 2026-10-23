#pragma once

#include <string>
#include <vector>

namespace livedelay {

// One row of the platform server combo. `is_auto` is "Automatycznie (zalecane)",
// whose URL is a copy of the closest ingest — so matching on URL alone cannot
// tell that row from the ingest itself.
struct ServerRow {
	bool is_auto = false;
	std::string name;
	std::string url;
};

// Index of the row to show, or -1 when `url` is a custom address.
// `pick` is the value saved as `server =` ("auto", a server name, or empty).
inline int PickServerRow(const std::vector<ServerRow> &rows, const std::string &pick, const std::string &url)
{
	if (pick == "auto") {
		for (size_t i = 0; i < rows.size(); ++i)
			if (rows[i].is_auto)
				return int(i);
	}
	if (!pick.empty() && pick != "auto") {
		for (size_t i = 0; i < rows.size(); ++i)
			if (!rows[i].is_auto && rows[i].name == pick)
				return int(i);
	}
	int auto_hit = -1;
	for (size_t i = 0; i < rows.size(); ++i) {
		if (url.empty() || rows[i].url != url)
			continue;
		if (!rows[i].is_auto)
			return int(i);
		if (auto_hit < 0)
			auto_hit = int(i);
	}
	return auto_hit;
}

} // namespace livedelay
