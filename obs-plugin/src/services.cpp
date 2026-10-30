#include "services.hpp"
#include "i18n.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace livedelay {

namespace {

QJsonObject ReadJson(const char *path)
{
	if (!path)
		return {};
	QFile f(QString::fromUtf8(path));
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QJsonDocument::fromJson(f.readAll()).object();
}

// OBS ships services.json with rtmp-services and keeps a newer copy in its config
// folder when it has downloaded an update; use whichever is newer.
QJsonObject ObsServiceList()
{
	obs_module_t *mod = obs_get_module("rtmp-services");
	if (!mod)
		return {};
	char *shipped = obs_find_module_file(mod, "services.json");
	char *updated = obs_module_get_config_path(mod, "services.json");
	QJsonObject a = ReadJson(shipped), b = ReadJson(updated);
	bfree(shipped);
	bfree(updated);
	if (b.value("services").toArray().isEmpty())
		return a;
	if (a.value("services").toArray().isEmpty())
		return b;
	return b.value("version").toInt() >= a.value("version").toInt() ? b : a;
}

bool IsRtmp(const QString &url)
{
	return url.startsWith("rtmp://") || url.startsWith("rtmps://");
}

// Replaces the static server list with OBS's cached ingest list (sorted by priority,
// i.e. closest first). "Auto" uses the first entry, which is what OBS's Auto does.
void UseIngestList(Service &s, const char *file)
{
	obs_module_t *mod = obs_get_module("rtmp-services");
	char *path = mod ? obs_module_get_config_path(mod, file) : nullptr;
	QJsonArray ingests = ReadJson(path).value("ingests").toArray();
	bfree(path);
	QVector<QPair<int, Server>> found;
	for (const QJsonValue &v : ingests) {
		QJsonObject o = v.toObject();
		QString url = o.value("url_template").toString();
		url.remove("/{stream_key}");
		if (IsRtmp(url))
			found.push_back({o.value("priority").toInt(), {o.value("name").toString(), url}});
	}
	if (found.isEmpty())
		return; // no cache yet: keep the list from services.json
	std::stable_sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
	s.servers.clear();
	s.servers.push_back({T("Settings.AutoServer"), found.front().second.url});
	for (const auto &f : found)
		s.servers.push_back(f.second);
}

QVector<Service> Load()
{
	QVector<Service> list;
	for (const QJsonValue &v : ObsServiceList().value("services").toArray()) {
		QJsonObject o = v.toObject();
		Service s;
		s.name = o.value("name").toString();
		s.common = o.value("common").toBool();
		s.key_link = o.value("stream_key_link").toString();
		const QJsonObject rec = o.value("recommended").toObject();
		s.max_video_kbps = rec.value("max video bitrate").toInt();
		s.max_audio_kbps = rec.value("max audio bitrate").toInt();
		s.keyint_sec = rec.value("keyint").toInt();
		for (const QJsonValue &sv : o.value("servers").toArray()) {
			QJsonObject so = sv.toObject();
			Server srv{so.value("name").toString(), so.value("url").toString()};
			if (IsRtmp(srv.url))
				s.servers.push_back(srv);
		}
		// Twitch and Amazon IVS servers come from the ingest lists OBS downloads,
		// already sorted closest first, exactly as Settings → Stream shows them.
		if (s.name == "Twitch")
			UseIngestList(s, "twitch_ingests.json");
		else if (s.name == "Amazon IVS")
			UseIngestList(s, "amazon_ivs_ingests.json");
		if (s.name.isEmpty() || s.servers.isEmpty())
			continue;
		list.push_back(s);
	}
	auto has = [&](const char *n) {
		return std::any_of(list.begin(), list.end(), [&](const Service &s) { return s.name == n; });
	};
	// Kick hands every channel its own ingest host, so there is no address to
	// ship: a shared one accepts the connection and then drops the stream.
	if (!has("Kick"))
		list.push_back({"Kick", {{T("Settings.ServerFromDashboard"), ""}},
				"https://dashboard.kick.com/settings/stream", true, 8000, 320, 2});
	if (!has("Twitch"))
		list.push_back({"Twitch", {{T("Settings.AutoServer"), "rtmp://live.twitch.tv/app"}},
				"https://dashboard.twitch.tv/settings/stream", true, 6000, 320, 2});
	if (!has("YouTube - RTMPS"))
		list.push_back({"YouTube - RTMPS", {{"Primary", "rtmps://a.rtmps.youtube.com:443/live2"}},
				"https://studio.youtube.com/channel/UC/livestreaming", true, 51000, 160, 2});
	std::stable_sort(list.begin(), list.end(), [](const Service &a, const Service &b) {
		if (a.common != b.common)
			return a.common;
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return list;
}

} // namespace

const QVector<Service> &Services()
{
	static const QVector<Service> list = Load();
	return list;
}

const Service *FindService(const QString &name)
{
	for (const Service &s : Services())
		if (s.name == name)
			return &s;
	return nullptr;
}

const Service *ServiceForUrl(const QString &url)
{
	for (const Service &s : Services())
		for (const Server &srv : s.servers)
			if (srv.url == url)
				return &s;
	return nullptr;
}

} // namespace livedelay
