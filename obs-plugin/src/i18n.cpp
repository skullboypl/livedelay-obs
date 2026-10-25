#include "i18n.hpp"
#include "settings.hpp"
#include "version.hpp"

#include <obs-frontend-api.h>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaObject>
#include <QThread>
#include <QUrl>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <cstring>
#include <unordered_map>

namespace livedelay {
namespace {

// Keys added after the locale files that a broken update can leave behind.
// If any of these comes back as itself, the installed ini is incomplete.
const char *kProbes[] = {
	"Settings.TabHotkeys", "Settings.TabLogs", "Hotkey.None", "Hotkey.Clear", "Hotkey.About",
	"Update.ProgressTitle", "Update.ProgressWaiting", "Encode.VodAudioHelp", "Settings.StreamDeckText",
	"Settings.TabAudio", "Encode.VodTrack", "WhatsNew.Title", "WhatsNew.Body1031",
};

std::unordered_map<std::string, std::string> overlay;

bool LookupMissing(const char *key)
{
	if (overlay.find(key) != overlay.end())
		return false;
	const char *s = obs_module_text(key);
	return !s || std::strcmp(s, key) == 0;
}

bool LocaleHasGaps()
{
	for (const char *key : kProbes) {
		if (LookupMissing(key))
			return true;
	}
	return false;
}

QString ActiveLanguage()
{
	const std::string configured = Settings::Load(ConfigPath()).language;
	if (!configured.empty() && configured != "auto")
		return QString::fromStdString(configured);
	const char *obs = obs_get_locale();
	return QString::fromUtf8(obs && *obs ? obs : "en-US");
}

QString GapNotice(const QString &lang)
{
	const QString l = lang.toLower();
	if (l.startsWith(QLatin1String("pl")))
		return QString::fromUtf8("Wykryto braki w tłumaczeniach. Spróbuj zaktualizować wtyczkę ponownie.");
	if (l.startsWith(QLatin1String("de")))
		return QString::fromUtf8("Es fehlen Übersetzungen. Versuche, das Plugin erneut zu aktualisieren.");
	if (l.startsWith(QLatin1String("ru")))
		return QString::fromUtf8("Не хватает переводов. Попробуйте обновить плагин ещё раз.");
	return QStringLiteral("Some translations are missing. Try updating the plugin again.");
}

bool SafeKey(const QString &key)
{
	if (key.isEmpty() || key.size() > 80)
		return false;
	for (const QChar c : key) {
		if (!(c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_')))
			return false;
	}
	return true;
}

QString EscIni(QString v)
{
	v.replace('\\', "\\\\");
	v.replace('"', "\\\"");
	v.replace('\r', QString());
	v.replace('\n', "\\n");
	return v;
}

bool HttpGetLimited(const QString &urlStr, QByteArray *out, QString *err)
{
	out->clear();
	const QUrl url(urlStr);
	if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host() != QLatin1String("livedelay.vxh.pl")) {
		if (err)
			*err = QStringLiteral("bad url");
		return false;
	}
	const std::wstring host = url.host().toStdWString();
	const std::wstring object = (url.path().isEmpty() ? QStringLiteral("/") : url.path()).toStdWString();
	const std::wstring ua = L"livedelay-obs/" + QStringLiteral(LD_VERSION_STRING).toStdWString();

	HINTERNET session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		if (err)
			*err = QStringLiteral("WinHttpOpen");
		return false;
	}
	WinHttpSetTimeouts(session, 4000, 4000, 8000, 8000);
	HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!connect) {
		WinHttpCloseHandle(session);
		if (err)
			*err = QStringLiteral("WinHttpConnect");
		return false;
	}
	HINTERNET request = WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
					       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!request) {
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		if (err)
			*err = QStringLiteral("WinHttpOpenRequest");
		return false;
	}
	const BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			  WinHttpReceiveResponse(request, nullptr);
	DWORD status = 0, statusSize = sizeof(status);
	if (sent)
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
				    &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
	if (sent && status >= 200 && status < 300) {
		for (;;) {
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
				break;
			QByteArray chunk(int(avail), 0);
			DWORD read = 0;
			if (!WinHttpReadData(request, chunk.data(), avail, &read))
				break;
			chunk.resize(int(read));
			out->append(chunk);
		}
	}
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);
	if (!sent || status < 200 || status >= 300) {
		if (err)
			*err = QStringLiteral("HTTP %1").arg(status);
		out->clear();
		return false;
	}
	return true;
}

// Fills overlay (and the on-disk ini) with keys the installed file does not have.
int ApplyLocaleBody(const QString &lang, const QByteArray &primary, const QByteArray &fallback)
{
	QJsonObject obj = QJsonDocument::fromJson(fallback).object();
	const QJsonObject over = QJsonDocument::fromJson(primary).object();
	for (auto it = over.begin(); it != over.end(); ++it)
		obj.insert(it.key(), it.value());

	QString extra;
	int filled = 0;
	for (auto it = obj.begin(); it != obj.end(); ++it) {
		if (!SafeKey(it.key()) || !it.value().isString())
			continue;
		const QByteArray key = it.key().toUtf8();
		if (!LookupMissing(key.constData()))
			continue;
		const QString value = it.value().toString();
		overlay[key.toStdString()] = value.toStdString();
		extra += it.key() + "=\"" + EscIni(value) + "\"\n";
		++filled;
	}
	if (filled == 0)
		return 0;

	const char *data = obs_get_module_data_path(obs_current_module());
	if (data && *data) {
		const QString dir = QString::fromUtf8(data) + "/locale";
		QDir().mkpath(dir);
		QFile file(dir + "/" + lang + ".ini");
		if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
			if (file.size() > 0)
				file.write("\n");
			file.write(extra.toUtf8());
			file.close();
			obs_module_set_locale(lang.toUtf8().constData());
		} else {
			blog(LOG_WARNING, "[livedelay] could not write locale %s", file.fileName().toUtf8().constData());
		}
	}
	blog(LOG_INFO, "[livedelay] filled %d missing translations for %s", filled, lang.toUtf8().constData());
	return filled;
}

} // namespace

QString T(const char *key)
{
	if (!key)
		return {};
	const auto it = overlay.find(key);
	if (it != overlay.end())
		return QString::fromStdString(it->second);
	const char *s = obs_module_text(key);
	return QString::fromUtf8(s ? s : key);
}

void StartLocaleHeal(std::function<void()> refreshed)
{
	if (!LocaleHasGaps())
		return;
	const QString lang = ActiveLanguage();
	QThread *thread = QThread::create([lang, refreshed] {
		QByteArray primary, fallback;
		QString err;
		const bool got = HttpGetLimited(QStringLiteral("https://livedelay.vxh.pl/locale/%1.json").arg(lang), &primary, &err);
		if (!got)
			blog(LOG_WARNING, "[livedelay] locale fetch failed: %s", err.toUtf8().constData());
		if (lang != QLatin1String("en-US"))
			HttpGetLimited(QStringLiteral("https://livedelay.vxh.pl/locale/en-US.json"), &fallback, nullptr);
		if (!qApp)
			return;
		QMetaObject::invokeMethod(
			qApp,
			[lang, refreshed, primary, fallback] {
				const int filled = ApplyLocaleBody(lang, primary, fallback);
				if (filled > 0 && refreshed)
					refreshed();
				if (LocaleHasGaps()) {
					QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
					QMessageBox::warning(parent, QStringLiteral("LiveDelay"), GapNotice(lang));
				}
			},
			Qt::QueuedConnection);
	});
	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	thread->start();
}

} // namespace livedelay
