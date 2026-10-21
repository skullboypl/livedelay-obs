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

QVector<Service> Load()
{
	QVector<Service> list;
	for (const QJsonValue &v : ObsServiceList().value("services").toArray()) {
		QJsonObject o = v.toObject();
		Service s;
		s.name = o.value("name").toString();
		s.common = o.value("common").toBool();
		s.key_link = o.value("stream_key_link").toString();
		for (const QJsonValue &sv : o.value("servers").toArray()) {
			QJsonObject so = sv.toObject();
			Server srv{so.value("name").toString(), so.value("url").toString()};
			if (IsRtmp(srv.url))
				s.servers.push_back(srv);
		}
		if (s.name.isEmpty() || s.servers.isEmpty())
			continue;
		if (s.name == "Twitch")
			// OBS picks the closest Twitch ingest itself ("Auto"); live.twitch.tv does the same by DNS.
			s.servers.prepend({T("Settings.AutoServer"), "rtmp://live.twitch.tv/app"});
		list.push_back(s);
	}
	auto has = [&](const char *n) {
		return std::any_of(list.begin(), list.end(), [&](const Service &s) { return s.name == n; });
	};
	if (!has("Kick"))
		list.push_back({"Kick", {{"Default", "rtmps://fa723fc1b171.global-contribute.live-video.net/app"}},
				"https://dashboard.kick.com/settings/stream", true});
	if (!has("Twitch"))
		list.push_back({"Twitch", {{T("Settings.AutoServer"), "rtmp://live.twitch.tv/app"}},
				"https://dashboard.twitch.tv/settings/stream", true});
	if (!has("YouTube - RTMPS"))
		list.push_back({"YouTube - RTMPS", {{"Primary", "rtmps://a.rtmps.youtube.com:443/live2"}},
				"https://studio.youtube.com/channel/UC/livestreaming", true});
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
