#include "presence.hpp"
#include "version.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QApplication>
#include <QFile>
#include <QTimer>
#include <QUuid>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace livedelay {
namespace {

constexpr const wchar_t *kHost = L"livedelay.vxh.pl";
constexpr const wchar_t *kPath = L"/api/heartbeat";

std::atomic<bool> g_stop{false};
std::atomic<bool> g_busy{false};
QTimer *g_timer = nullptr;

QString InstallId()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *raw = obs_module_config_path("install.id");
	const QString path = QString::fromUtf8(raw ? raw : "");
	bfree(raw);

	QFile file(path);
	if (file.open(QIODevice::ReadOnly)) {
		const QString existing = QString::fromUtf8(file.readAll()).trimmed();
		file.close();
		if (existing.size() >= 16 && existing.size() <= 64)
			return existing;
	}
	const QString created = QUuid::createUuid().toString(QUuid::Id128);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		file.write(created.toUtf8());
	return created;
}

void PostHeartbeat(const QString &id)
{
	if (g_stop.load() || g_busy.exchange(true))
		return;
	std::thread([id] {
		struct Busy {
			~Busy() { g_busy.store(false); }
		} busy;
		if (g_stop.load())
			return;

		const QByteArray body = QByteArray("{\"id\":\"") + id.toUtf8() + "\"}";
		const std::wstring ua = L"livedelay-obs/" + QStringLiteral(LD_VERSION_STRING).toStdWString();
		HINTERNET session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
						WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
			return;
		WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
		HINTERNET connect = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
		if (!connect) {
			WinHttpCloseHandle(session);
			return;
		}
		HINTERNET request = WinHttpOpenRequest(connect, L"POST", kPath, nullptr, WINHTTP_NO_REFERER,
						       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (!request) {
			WinHttpCloseHandle(connect);
			WinHttpCloseHandle(session);
			return;
		}
		const wchar_t *headers = L"Content-Type: application/json\r\n";
		WinHttpSendRequest(request, headers, (ULONG)-1L, (LPVOID)body.constData(), (DWORD)body.size(),
				   (DWORD)body.size(), 0);
		WinHttpReceiveResponse(request, nullptr);
		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
	}).detach();
}

} // namespace

void StartPresence()
{
	g_stop.store(false);
	const QString id = InstallId();
	g_timer = new QTimer(qApp);
	g_timer->setInterval(60 * 1000);
	QObject::connect(g_timer, &QTimer::timeout, qApp, [id] { PostHeartbeat(id); });
	g_timer->start();
	QTimer::singleShot(8000, qApp, [id] { PostHeartbeat(id); });
}

void StopPresence()
{
	g_stop.store(true);
	if (g_timer) {
		g_timer->stop();
		g_timer->deleteLater();
		g_timer = nullptr;
	}
}

} // namespace livedelay
