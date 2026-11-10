#include "panels.hpp"
#include "i18n.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUuid>

namespace livedelay {

namespace {

// Addresses checked by hand against each platform. A dashboard asks the viewer
// to sign in; that happens inside the dock, in OBS's own cookie jar.
const BrowserPanel kTwitch[] = {
	// The three a streamer reaches for: read chat, see who followed, change the
	// title and the category. Each has its own popout, so each is its own dock.
	{"twitch.chat", "Panels.Chat", "https://www.twitch.tv/popout/{channel}/chat?darkpopout", true, true},
	{"twitch.activity", "Panels.Activity",
	 "https://dashboard.twitch.tv/popout/u/{channel}/stream-manager/activity-feed", true, true},
	{"twitch.info", "Panels.StreamInfo",
	 "https://dashboard.twitch.tv/popout/u/{channel}/stream-manager/edit-stream-info", true, true},
	// There if wanted, off unless ticked.
	{"twitch.manager", "Panels.StreamManager", "https://dashboard.twitch.tv/u/{channel}/stream-manager", true, false},
	{"twitch.moderation", "Panels.Moderation", "https://www.twitch.tv/moderator/{channel}", true, false},
	// /analytics/stream bounces to the dashboard home; this is the real page.
	{"twitch.analytics", "Panels.Analytics", "https://dashboard.twitch.tv/u/{channel}/analytics/stream-summary",
	 true, false},
};

const BrowserPanel kKick[] = {
	{"kick.chat", "Panels.Chat", "https://kick.com/popout/{channel}/chat", true, true},
	// Kick keeps chat, the activity list and the title in one page, and
	// kick.com/dashboard/stream only redirects here, so dial the real address.
	{"kick.dashboard", "Panels.Dashboard", "https://dashboard.kick.com/", false, true},
};

// YouTube hands out no per-channel chat address: the chat page is keyed to the
// broadcast that is live right now, so there is nothing stable to save. Studio
// is what is left, and it lands on whichever channel is signed in.
const BrowserPanel kYouTube[] = {
	{"youtube.studio", "Panels.YouTubeStudio", "https://studio.youtube.com/", false, true},
};

template<size_t N> QVector<BrowserPanel> Vec(const BrowserPanel (&panels)[N])
{
	QVector<BrowserPanel> out;
	out.reserve(int(N));
	for (size_t i = 0; i < N; ++i)
		out.push_back(panels[i]);
	return out;
}

QVector<QString> g_titles;
QVector<QString> g_urls;

QString NewUuid()
{
	QString uuid = QUuid::createUuid().toString();
	uuid.remove(QRegularExpression(QStringLiteral("[{}-]")));
	return uuid;
}

// Merges the queue into OBS's own list and writes it out. Runs from the exit
// event, which OBS raises after it has already saved that list, so what is on
// disk at this point is every dock the user had, ours included from last time.
void FlushBrowserDocks()
{
	if (g_urls.isEmpty())
		return;
	config_t *cfg = obs_frontend_get_user_config();
	if (!cfg) {
		blog(LOG_WARNING, "[livedelay] no user config; browser panels not saved");
		return;
	}
	const char *existing = config_get_string(cfg, "BasicWindow", "ExtraBrowserDocks");
	QJsonArray docks;
	if (existing && *existing) {
		const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(existing));
		if (doc.isArray())
			docks = doc.array();
	}
	int added = 0;
	for (int i = 0; i < g_urls.size(); ++i) {
		bool already = false;
		for (const QJsonValue &value : docks) {
			if (value.toObject().value("url").toString() == g_urls[i]) {
				already = true;
				break;
			}
		}
		if (already)
			continue;
		QJsonObject dock;
		dock["title"] = g_titles[i];
		dock["url"] = g_urls[i];
		dock["uuid"] = NewUuid();
		docks.append(dock);
		++added;
	}
	g_titles.clear();
	g_urls.clear();
	if (added == 0)
		return;
	const QByteArray json = QJsonDocument(docks).toJson(QJsonDocument::Compact);
	config_set_string(cfg, "BasicWindow", "ExtraBrowserDocks", json.constData());
	// Nothing saves the user config after this point, so write it ourselves.
	if (config_save_safe(cfg, "tmp", nullptr) != CONFIG_SUCCESS)
		blog(LOG_WARNING, "[livedelay] could not save browser panels");
	else
		blog(LOG_INFO, "[livedelay] %d browser panel(s) added for the next start", added);
}

void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		FlushBrowserDocks();
}

} // namespace

QVector<BrowserPanel> PanelsForService(const QString &service)
{
	if (service.contains(QLatin1String("Twitch"), Qt::CaseInsensitive))
		return Vec(kTwitch);
	if (service.contains(QLatin1String("Kick"), Qt::CaseInsensitive))
		return Vec(kKick);
	if (service.contains(QLatin1String("YouTube"), Qt::CaseInsensitive))
		return Vec(kYouTube);
	return {};
}

// Channel names on these platforms are letters, digits, _ and -. Keeping to
// that is simpler than escaping, and it stops a pasted address from turning
// into a different one.
QString CleanChannel(const QString &channel)
{
	QString out;
	for (const QChar c : channel.trimmed()) {
		if (c.isLetterOrNumber() && c.unicode() < 128)
			out += c;
		else if (c == QLatin1Char('_') || c == QLatin1Char('-'))
			out += c;
	}
	return out;
}

QString PanelAddress(const BrowserPanel &panel, const QString &channel)
{
	const QString clean = CleanChannel(channel);
	if (panel.needsChannel && clean.isEmpty())
		return {};
	return QString::fromLatin1(panel.url).replace(QStringLiteral("{channel}"), clean);
}

QString PanelTitle(const BrowserPanel &panel, const QString &destName)
{
	return destName.trimmed().isEmpty() ? T(panel.nameKey)
					    : QStringLiteral("%1 · %2").arg(destName.trimmed(), T(panel.nameKey));
}

void QueueBrowserDock(const QString &title, const QString &url)
{
	if (title.isEmpty() || url.isEmpty() || g_urls.contains(url))
		return;
	g_titles.push_back(title);
	g_urls.push_back(url);
}

int QueuedBrowserDockCount()
{
	return int(g_urls.size());
}

void InstallBrowserDockWriter()
{
	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
}

void RemoveBrowserDockWriter()
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
}

} // namespace livedelay
