// Auto (recommended) copies the closest ingest's URL. Picking it must stay
// on that row after a reload, and a file with no `server =` line must keep
// showing the named ingest instead of jumping back to Auto.
#include "serverpick.hpp"

#include <cstdio>
#include <string>
#include <vector>

int main()
{
	using livedelay::PickServerRow;
	using livedelay::ServerRow;
	const std::string waw = "rtmp://waw.contribute.live-video.net/app";
	const std::string fra = "rtmp://fra.contribute.live-video.net/app";
	const std::vector<ServerRow> rows = {
		{true, "Automatycznie (zalecane)", waw},
		{false, "Europe: Warsaw, PL", waw},
		{false, "Europe: Frankfurt, DE", fra},
	};
	int fails = 0;
	auto eq = [&](const char *name, int got, int want) {
		if (got != want) {
			std::printf("FAIL %s: got %d, want %d\n", name, got, want);
			++fails;
		}
	};
	eq("auto stays on auto", PickServerRow(rows, "auto", waw), 0);
	eq("named warsaw stays", PickServerRow(rows, "Europe: Warsaw, PL", waw), 1);
	eq("frankfurt stays", PickServerRow(rows, "Europe: Frankfurt, DE", fra), 2);
	eq("legacy shared url shows the ingest, not auto", PickServerRow(rows, "", waw), 1);
	eq("unknown url is custom", PickServerRow(rows, "", "rtmp://example/app"), -1);
	eq("empty everything is custom", PickServerRow(rows, "", ""), -1);
	if (fails)
		return 1;
	std::printf("serverpick: ok\n");
	return 0;
}
