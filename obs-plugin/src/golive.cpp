#include "golive.hpp"

#include <obs-module.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QThread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>
#include <winhttp.h>

#include <vector>

namespace livedelay {
namespace {

struct UrlParts {
	std::wstring host;
	std::wstring path;
	bool https = true;
};

bool SplitUrl(const QString &url, UrlParts &out)
{
	const QUrl u(url);
	if (!u.isValid() || u.host().isEmpty())
		return false;
	out.https = u.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
	out.host = u.host().toStdWString();
	QString path = u.path();
	if (path.isEmpty())
		path = QStringLiteral("/");
	if (u.hasQuery())
		path += QStringLiteral("?") + u.query();
	out.path = path.toStdWString();
	return true;
}

bool HttpsPost(const QString &url, const QByteArray &body, QByteArray &response, std::string &error)
{
	UrlParts parts;
	if (!SplitUrl(url, parts) || !parts.https) {
		error = "config url is not https";
		return false;
	}
	const std::wstring ua = L"obs-studio";
	HINTERNET session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		error = "WinHttpOpen";
		return false;
	}
	WinHttpSetTimeouts(session, 4000, 4000, 8000, 8000);
	HINTERNET connect = WinHttpConnect(session, parts.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (!connect) {
		WinHttpCloseHandle(session);
		error = "WinHttpConnect";
		return false;
	}
	HINTERNET request = WinHttpOpenRequest(connect, L"POST", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
					       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!request) {
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		error = "WinHttpOpenRequest";
		return false;
	}
	const wchar_t *headers = L"Content-Type: application/json\r\n";
	const BOOL sent = WinHttpSendRequest(request, headers, (DWORD)-1L, (LPVOID)body.constData(),
					     (DWORD)body.size(), (DWORD)body.size(), 0) &&
			  WinHttpReceiveResponse(request, nullptr);
	bool ok = false;
	if (sent) {
		DWORD status = 0;
		DWORD size = sizeof(status);
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		if (status >= 200 && status < 300) {
			DWORD avail = 0;
			while (WinHttpQueryDataAvailable(request, &avail) && avail > 0 && response.size() < 1024 * 1024) {
				QByteArray chunk(int(avail), 0);
				DWORD read = 0;
				if (!WinHttpReadData(request, chunk.data(), avail, &read))
					break;
				response.append(chunk.constData(), int(read));
				if (read == 0)
					break;
			}
			ok = !response.isEmpty();
			if (!ok)
				error = "empty GoLive response";
		} else {
			error = "GoLive HTTP " + std::to_string(status);
		}
	} else {
		error = "WinHttp request " + std::to_string(GetLastError());
	}
	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);
	return ok;
}

int PhysicalCores()
{
	DWORD len = 0;
	GetLogicalProcessorInformation(nullptr, &len);
	if (len == 0)
		return int(QThread::idealThreadCount());
	std::vector<char> buf(len);
	if (!GetLogicalProcessorInformation(reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buf.data()), &len))
		return int(QThread::idealThreadCount());
	int cores = 0;
	const auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buf.data());
	const size_t n = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
	for (size_t i = 0; i < n; ++i) {
		if (info[i].Relationship == RelationProcessorCore)
			++cores;
	}
	return cores > 0 ? cores : int(QThread::idealThreadCount());
}

void AddGpu(QJsonArray &gpus)
{
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
		return;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
		DXGI_ADAPTER_DESC desc{};
		if (FAILED(adapter->GetDesc(&desc))) {
			adapter->Release();
			adapter = nullptr;
			continue;
		}
		if (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c) {
			adapter->Release();
			adapter = nullptr;
			continue;
		}
		QJsonObject gpu;
		gpu.insert(QStringLiteral("model"), QString::fromWCharArray(desc.Description));
		gpu.insert(QStringLiteral("vendor_id"), int(desc.VendorId));
		gpu.insert(QStringLiteral("device_id"), int(desc.DeviceId));
		gpu.insert(QStringLiteral("dedicated_video_memory"), double(desc.DedicatedVideoMemory));
		gpu.insert(QStringLiteral("shared_system_memory"), double(desc.SharedSystemMemory));
		gpus.append(gpu);
		adapter->Release();
		adapter = nullptr;
	}
	factory->Release();
}

QByteArray PostBody(const Destination &dest)
{
	QJsonObject post;
	post.insert(QStringLiteral("service"), QStringLiteral("IVS"));
	post.insert(QStringLiteral("schema_version"), QStringLiteral("2024-06-04"));
	post.insert(QStringLiteral("authentication"), QString::fromStdString(dest.key));

	QJsonObject client;
	client.insert(QStringLiteral("name"), QStringLiteral("obs-studio"));
	client.insert(QStringLiteral("version"), QString::fromUtf8(obs_get_version_string()));
	QJsonArray codecs;
	const char *encoder_id = nullptr;
	bool h264 = false;
	bool hevc = false;
	bool av1 = false;
	for (size_t i = 0; obs_enum_encoder_types(i, &encoder_id); ++i) {
		const char *codec = obs_get_encoder_codec(encoder_id);
		if (!codec)
			continue;
		const QString c = QString::fromUtf8(codec).toLower();
		if (c == QStringLiteral("h264"))
			h264 = true;
		else if (c == QStringLiteral("hevc") || c == QStringLiteral("h265"))
			hevc = true;
		else if (c == QStringLiteral("av1"))
			av1 = true;
	}
	if (h264)
		codecs.append(QStringLiteral("h264"));
	if (hevc)
		codecs.append(QStringLiteral("h265"));
	if (av1)
		codecs.append(QStringLiteral("av1"));
	if (codecs.isEmpty())
		codecs.append(QStringLiteral("h264"));
	client.insert(QStringLiteral("supported_codecs"), codecs);
	post.insert(QStringLiteral("client"), client);

	QJsonObject cpu;
	cpu.insert(QStringLiteral("physical_cores"), PhysicalCores());
	cpu.insert(QStringLiteral("logical_cores"), QThread::idealThreadCount());
	MEMORYSTATUSEX mem{};
	mem.dwLength = sizeof(mem);
	QJsonObject memory;
	if (GlobalMemoryStatusEx(&mem)) {
		memory.insert(QStringLiteral("total"), double(mem.ullTotalPhys));
		memory.insert(QStringLiteral("free"), double(mem.ullAvailPhys));
	} else {
		memory.insert(QStringLiteral("total"), 0);
		memory.insert(QStringLiteral("free"), 0);
	}
	OSVERSIONINFOEXW ver{};
	ver.dwOSVersionInfoSize = sizeof(ver);
	using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
	if (auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
		rtl(reinterpret_cast<PRTL_OSVERSIONINFOW>(&ver));
	QJsonObject system;
	system.insert(QStringLiteral("version"),
		      QStringLiteral("%1.%2.%3").arg(ver.dwMajorVersion).arg(ver.dwMinorVersion).arg(ver.dwBuildNumber));
	system.insert(QStringLiteral("name"), QStringLiteral("Windows"));
	system.insert(QStringLiteral("build"), int(ver.dwBuildNumber));
	system.insert(QStringLiteral("release"), QString::number(ver.dwMajorVersion));
	system.insert(QStringLiteral("revision"), 0);
	system.insert(QStringLiteral("bits"), 64);
	system.insert(QStringLiteral("arm"), false);
	system.insert(QStringLiteral("armEmulation"), false);
	QJsonObject caps;
	caps.insert(QStringLiteral("cpu"), cpu);
	caps.insert(QStringLiteral("memory"), memory);
	caps.insert(QStringLiteral("system"), system);
	QJsonArray gpus;
	AddGpu(gpus);
	if (!gpus.isEmpty())
		caps.insert(QStringLiteral("gpu"), gpus);
	post.insert(QStringLiteral("capabilities"), caps);

	QJsonObject pref;
	pref.insert(QStringLiteral("vod_track_audio"), dest.vod_track > 0 || dest.vod_audio);
	obs_video_info ovi{};
	if (obs_get_video_info(&ovi)) {
		pref.insert(QStringLiteral("width"), int(ovi.output_width));
		pref.insert(QStringLiteral("height"), int(ovi.output_height));
		QJsonObject fps;
		fps.insert(QStringLiteral("numerator"), int(ovi.fps_num));
		fps.insert(QStringLiteral("denominator"), int(ovi.fps_den));
		pref.insert(QStringLiteral("framerate"), fps);
		pref.insert(QStringLiteral("canvas_width"), int(ovi.base_width));
		pref.insert(QStringLiteral("canvas_height"), int(ovi.base_height));
		pref.insert(QStringLiteral("composition_gpu_index"), int(ovi.adapter));
	}
	post.insert(QStringLiteral("preferences"), pref);
	return QJsonDocument(post).toJson(QJsonDocument::Compact);
}

QString ConfigUrl(const Destination &dest)
{
	const QString url = QString::fromStdString(dest.url);
	if (url.contains(QStringLiteral("live-video.net"), Qt::CaseInsensitive))
		return QStringLiteral("https://ingest.contribute.live-video.net/api/v3/GetClientConfiguration");
	return QStringLiteral("https://ingest.twitch.tv/api/v3/GetClientConfiguration");
}

} // namespace

bool FetchGoLive(const Destination &dest, GoLivePlan &out, std::string &error)
{
	out = {};
	if (dest.key.empty()) {
		error = "no stream key";
		return false;
	}
	QByteArray response;
	if (!HttpsPost(ConfigUrl(dest), PostBody(dest), response, error))
		return false;
	const QJsonDocument doc = QJsonDocument::fromJson(response);
	if (!doc.isObject()) {
		error = "GoLive response is not JSON";
		return false;
	}
	const QJsonObject root = doc.object();
	const QString status = root.value(QStringLiteral("status")).toObject().value(QStringLiteral("result")).toString();
	if (status == QStringLiteral("error")) {
		error = "Twitch refused the GoLive session";
		return false;
	}
	const QJsonArray rungs = root.value(QStringLiteral("encoder_configurations")).toArray();
	if (rungs.isEmpty()) {
		error = "GoLive returned no video ladder";
		return false;
	}
	for (const QJsonValue &v : rungs) {
		const QJsonObject o = v.toObject();
		GoLiveRung rung;
		rung.type = o.value(QStringLiteral("type")).toString().toStdString();
		rung.width = uint32_t(o.value(QStringLiteral("width")).toInt());
		rung.height = uint32_t(o.value(QStringLiteral("height")).toInt());
		const QJsonObject fps = o.value(QStringLiteral("framerate")).toObject();
		rung.fps_num = uint32_t(fps.value(QStringLiteral("numerator")).toInt());
		rung.fps_den = uint32_t(fps.value(QStringLiteral("denominator")).toInt());
		rung.scale = o.value(QStringLiteral("gpu_scale_type")).toString().toStdString();
		const QJsonValue settings = o.value(QStringLiteral("settings"));
		if (settings.isObject())
			rung.settings_json = QJsonDocument(settings.toObject()).toJson(QJsonDocument::Compact).toStdString();
		else
			rung.settings_json = "{}";
		if (!rung.type.empty())
			out.rungs.push_back(std::move(rung));
	}
	if (out.rungs.empty()) {
		error = "GoLive ladder had no encoder type";
		return false;
	}

	QString server;
	QString auth;
	for (const QJsonValue &v : root.value(QStringLiteral("ingest_endpoints")).toArray()) {
		const QJsonObject ep = v.toObject();
		if (!ep.value(QStringLiteral("protocol")).toString().startsWith(QStringLiteral("RTMP"), Qt::CaseInsensitive))
			continue;
		server = ep.value(QStringLiteral("url_template")).toString();
		auth = ep.value(QStringLiteral("authentication")).toString();
		break;
	}
	if (server.isEmpty()) {
		error = "GoLive returned no RTMP ingest";
		return false;
	}
	const int placeholder = server.indexOf(QStringLiteral("/{stream_key}"));
	if (placeholder >= 0)
		server.truncate(placeholder);
	while (server.endsWith('/'))
		server.chop(1);

	QString key = auth.isEmpty() ? QString::fromStdString(dest.key) : auth;
	const QString user = QString::fromStdString(dest.key);
	const int uq = user.indexOf('?');
	if (uq >= 0) {
		const QString q = user.mid(uq + 1);
		if (!q.isEmpty() && !key.contains(q))
			key += (key.contains('?') ? QStringLiteral("&") : QStringLiteral("?")) + q;
	}
	const QString configId = root.value(QStringLiteral("meta")).toObject().value(QStringLiteral("config_id")).toString();
	if (!configId.isEmpty())
		key += (key.contains('?') ? QStringLiteral("&") : QStringLiteral("?")) + QStringLiteral("clientConfigId=") +
		       configId;
	out.url = server.toStdString();
	out.key = key.toStdString();
	blog(LOG_INFO, "[livedelay] GoLive %s rungs=%zu config=%s", server.toUtf8().constData(), out.rungs.size(),
	     configId.toUtf8().constData());
	return !out.url.empty() && !out.key.empty();
}

} // namespace livedelay
