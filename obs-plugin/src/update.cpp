#include "update.hpp"
#include "i18n.hpp"
#include "version.hpp"

Q_DECLARE_METATYPE(livedelay::UpdateInfo)

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace livedelay {
namespace {

constexpr const char *kGithubLatest = "https://api.github.com/repos/skullboypl/livedelay-obs/releases/latest";
constexpr const char *kMirrorJson = "https://livedelay.vxh.pl/version.json";
constexpr const char *kSkipKey = "update/skipVersion";

std::wstring UserAgent()
{
	return L"livedelay-obs/" + QStringLiteral(LD_VERSION_STRING).toStdWString() + L" (+https://livedelay.vxh.pl)";
}

// Qt Network SSL often fails inside OBS ("TLS initialization failed") because
// OpenSSL plugins are missing next to obs64.exe. WinHTTP uses Windows Schannel.

bool HttpGet(const QString &urlStr, const wchar_t *extraHeaders, QByteArray *out, QString *err)
{
	out->clear();
	const QUrl url(urlStr);
	if (!url.isValid() || url.host().isEmpty()) {
		if (err)
			*err = QStringLiteral("bad url");
		return false;
	}

	const bool https = url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
	INTERNET_PORT port = url.port(https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
	const std::wstring host = url.host().toStdWString();
	QString path = url.path();
	if (path.isEmpty())
		path = QStringLiteral("/");
	if (url.hasQuery())
		path += QLatin1Char('?') + url.query();
	const std::wstring object = path.toStdWString();

	const std::wstring ua = UserAgent();
	HINTERNET session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
					WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		if (err)
			*err = QStringLiteral("WinHttpOpen %1").arg(GetLastError());
		return false;
	}

	HINTERNET connect = WinHttpConnect(session, host.c_str(), port, 0);
	if (!connect) {
		if (err)
			*err = QStringLiteral("WinHttpConnect %1").arg(GetLastError());
		WinHttpCloseHandle(session);
		return false;
	}

	DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET request = WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
					       WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
	if (!request) {
		if (err)
			*err = QStringLiteral("WinHttpOpenRequest %1").arg(GetLastError());
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		return false;
	}

	// Follow redirects (GitHub asset / API).
	DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
	WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));

	BOOL ok = TRUE;
	if (extraHeaders && *extraHeaders)
		ok = WinHttpAddRequestHeaders(request, extraHeaders, (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);
	if (ok)
		ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
	if (ok)
		ok = WinHttpReceiveResponse(request, nullptr);

	if (!ok) {
		if (err)
			*err = QStringLiteral("WinHttp request %1").arg(GetLastError());
		WinHttpCloseHandle(request);
		WinHttpCloseHandle(connect);
		WinHttpCloseHandle(session);
		return false;
	}

	DWORD status = 0, statusSize = sizeof(status);
	WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			    &status, &statusSize, WINHTTP_NO_HEADER_INDEX);

	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(request, &avail))
			break;
		if (avail == 0)
			break;
		QByteArray chunk;
		chunk.resize(int(avail));
		DWORD read = 0;
		if (!WinHttpReadData(request, chunk.data(), avail, &read))
			break;
		chunk.resize(int(read));
		out->append(chunk);
	}

	WinHttpCloseHandle(request);
	WinHttpCloseHandle(connect);
	WinHttpCloseHandle(session);

	if (status < 200 || status >= 300) {
		if (err)
			*err = QStringLiteral("HTTP %1").arg(status);
		return false;
	}
	return true;
}

bool NewerThan(const QString &remote, const QString &local)
{
	auto parts = [](QString v) {
		v.remove(QChar('v'), Qt::CaseInsensitive);
		const QStringList s = v.split('.', Qt::SkipEmptyParts);
		std::vector<int> out;
		const int n = qMax(3, s.size());
		for (int i = 0; i < n; i++)
			out.push_back(i < s.size() ? s[i].toInt() : 0);
		return out;
	};
	auto a = parts(remote), b = parts(local);
	const size_t n = qMax(a.size(), b.size());
	a.resize(n, 0);
	b.resize(n, 0);
	for (size_t i = 0; i < n; i++) {
		if (a[i] > b[i])
			return true;
		if (a[i] < b[i])
			return false;
	}
	return false;
}

QString StripV(QString v)
{
	if (v.startsWith('v', Qt::CaseInsensitive))
		v = v.mid(1);
	return v.trimmed();
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent)
{
	qRegisterMetaType<livedelay::UpdateInfo>("livedelay::UpdateInfo");
}

QString UpdateChecker::CurrentVersion()
{
	return QStringLiteral(LD_VERSION_STRING);
}

void UpdateChecker::SkipVersion(const QString &version)
{
	QSettings s("skullboypl", "livedelay-obs");
	s.setValue(kSkipKey, StripV(version));
}

bool UpdateChecker::IsSkipped(const QString &version)
{
	QSettings s("skullboypl", "livedelay-obs");
	return s.value(kSkipKey).toString() == StripV(version);
}

void UpdateChecker::OpenDownload(const UpdateInfo &info)
{
	const QUrl url(info.zipUrl.isEmpty() ? info.releaseUrl : info.zipUrl);
	if (url.isValid())
		QDesktopServices::openUrl(url);
}

namespace {

QString PluginDllPath()
{
	obs_module_t *mod = obs_current_module();
	const char *p = mod ? obs_get_module_binary_path(mod) : nullptr;
	return p ? QString::fromUtf8(p) : QString();
}

QString ObsExePath()
{
	wchar_t buf[MAX_PATH];
	if (!GetModuleFileNameW(nullptr, buf, MAX_PATH))
		return {};
	return QString::fromWCharArray(buf);
}

// Zip layout → ProgramData plugin OR Program Files OBS tree.
bool ResolveInstallDirs(const QString &dllPath, QString *dllDestDir, QString *dataDestDir, QString *err)
{
	const QFileInfo fi(dllPath);
	if (!fi.exists()) {
		*err = QStringLiteral("dll_missing");
		return false;
	}
	*dllDestDir = QDir::toNativeSeparators(fi.absolutePath());
	const QString native = QDir::toNativeSeparators(dllPath);
	// …\plugins\livedelay-obs\bin\64bit\livedelay-obs.dll
	if (native.contains(QStringLiteral("\\plugins\\livedelay-obs\\"), Qt::CaseInsensitive)) {
		QDir bin(fi.absolutePath()); // …/bin/64bit
		bin.cdUp(); // bin
		bin.cdUp(); // livedelay-obs
		*dataDestDir = QDir::toNativeSeparators(bin.filePath(QStringLiteral("data")));
		return true;
	}
	// …\obs-studio\obs-plugins\64bit\livedelay-obs.dll
	QDir plugins(fi.absolutePath()); // 64bit
	plugins.cdUp(); // obs-plugins
	plugins.cdUp(); // obs-studio root
	*dataDestDir = QDir::toNativeSeparators(plugins.filePath(QStringLiteral("data/obs-plugins/livedelay-obs")));
	return true;
}

QString PsQuote(const QString &s)
{
	QString o = s;
	o.replace('\'', QStringLiteral("''"));
	return QStringLiteral("'%1'").arg(o);
}

bool WriteUpdaterScript(const QString &scriptPath, DWORD obsPid, const QString &obsExe, const QString &extractDir,
			const QString &dllDestDir, const QString &dataDestDir, QString *err)
{
	// Zip has obs-plugins\64bit\*.dll and data\obs-plugins\livedelay-obs\*
	const QString dllSrc = QDir(extractDir).filePath(QStringLiteral("obs-plugins/64bit/livedelay-obs.dll"));
	const QString dataSrc = QDir(extractDir).filePath(QStringLiteral("data/obs-plugins/livedelay-obs"));
	const QString logPath = QDir::temp().filePath(QStringLiteral("livedelay-update.log"));

	QString ps;
	ps += QStringLiteral("$ErrorActionPreference='Stop'\n");
	ps += QStringLiteral("Start-Transcript -Path %1 -Force | Out-Null\n").arg(PsQuote(logPath));
	ps += QStringLiteral("$pidWait=%1\n").arg(obsPid);
	ps += QStringLiteral("$obsExe=%1\n").arg(PsQuote(QDir::toNativeSeparators(obsExe)));
	ps += QStringLiteral("$dllSrc=%1\n").arg(PsQuote(QDir::toNativeSeparators(dllSrc)));
	ps += QStringLiteral("$dllDst=%1\n").arg(PsQuote(QDir::toNativeSeparators(QDir(dllDestDir).filePath(QStringLiteral("livedelay-obs.dll")))));
	ps += QStringLiteral("$dataSrc=%1\n").arg(PsQuote(QDir::toNativeSeparators(dataSrc)));
	ps += QStringLiteral("$dataDst=%1\n").arg(PsQuote(QDir::toNativeSeparators(dataDestDir)));
	ps += QStringLiteral(
		"Write-Host 'Waiting for OBS to exit...'\n"
		"$deadline=(Get-Date).AddMinutes(3)\n"
		"while ((Get-Date) -lt $deadline) {\n"
		"  $alive=$false\n"
		"  try { $p=Get-Process -Id $pidWait -ErrorAction Stop; if ($p) { $alive=$true } } catch {}\n"
		"  Get-Process obs64,obs32 -ErrorAction SilentlyContinue | ForEach-Object { $alive=$true }\n"
		"  if (-not $alive) { break }\n"
		"  Start-Sleep -Milliseconds 400\n"
		"}\n"
		"Start-Sleep -Seconds 1\n"
		"if (-not (Test-Path -LiteralPath $dllSrc)) { throw \"missing $dllSrc\" }\n"
		"$dllDir=Split-Path -Parent $dllDst\n"
		"New-Item -ItemType Directory -Force -Path $dllDir,$dataDst | Out-Null\n"
		"Copy-Item -LiteralPath $dllSrc -Destination $dllDst -Force\n"
		"Copy-Item -LiteralPath (Join-Path $dataSrc '*') -Destination $dataDst -Recurse -Force\n"
		"Write-Host 'Installed. Starting OBS...'\n"
		"if (Test-Path -LiteralPath $obsExe) {\n"
		"  $wd=Split-Path -Parent $obsExe\n"
		"  Start-Process -FilePath $obsExe -WorkingDirectory $wd\n"
		"}\n"
		"Stop-Transcript | Out-Null\n");

	QFile f(scriptPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		*err = QStringLiteral("script_write");
		return false;
	}
	f.write(ps.toUtf8());
	f.close();
	return true;
}

} // namespace

void UpdateChecker::ApplyUpdate(const UpdateInfo &info, QWidget *parent)
{
	if (info.zipUrl.isEmpty()) {
		OpenDownload(info);
		return;
	}
	if (QMessageBox::question(parent, QStringLiteral("LiveDelay"), T("Update.Confirm").arg(info.version),
				  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		return;

	const QString dllPath = PluginDllPath();
	const QString obsExe = ObsExePath();
	QString dllDest, dataDest, err;
	if (dllPath.isEmpty() || !ResolveInstallDirs(dllPath, &dllDest, &dataDest, &err)) {
		QMessageBox::warning(parent, QStringLiteral("LiveDelay"), T("Update.ApplyFailed").arg(err.isEmpty() ? QStringLiteral("path") : err));
		OpenDownload(info);
		return;
	}

	auto *progress = new QMessageBox(parent);
	progress->setWindowTitle(QStringLiteral("LiveDelay"));
	progress->setText(T("Update.Downloading").arg(info.version));
	progress->setStandardButtons(QMessageBox::NoButton);
	progress->setModal(true);
	progress->show();

	const UpdateInfo infoCopy = info;
	const QString dllDestCopy = dllDest;
	const QString dataDestCopy = dataDest;
	const QString obsExeCopy = obsExe;
	const DWORD pid = GetCurrentProcessId();
	QPointer<QWidget> parentGuard(parent);
	QPointer<QMessageBox> progressGuard(progress);

	QThread *thread = QThread::create([infoCopy, dllDestCopy, dataDestCopy, obsExeCopy, pid, parentGuard, progressGuard] {
		QByteArray zip;
		QString netErr;
		const bool ok = HttpGet(infoCopy.zipUrl, nullptr, &zip, &netErr);
		QMetaObject::invokeMethod(
			qApp,
			[ok, zip, netErr, infoCopy, dllDestCopy, dataDestCopy, obsExeCopy, pid, parentGuard, progressGuard] {
				auto closeProgress = [&] {
					if (progressGuard)
						progressGuard->close();
					if (progressGuard)
						progressGuard->deleteLater();
				};
				QWidget *dlgParent = parentGuard ? parentGuard.data() : static_cast<QWidget *>(obs_frontend_get_main_window());
				if (!ok || zip.size() < 1000) {
					closeProgress();
					QMessageBox::warning(dlgParent, QStringLiteral("LiveDelay"),
							     T("Update.ApplyFailed").arg(netErr.isEmpty() ? QStringLiteral("download") : netErr));
					return;
				}

				const QString work = QDir::temp().filePath(QStringLiteral("livedelay-update-%1").arg(infoCopy.version));
				QDir().rmpath(work);
				QDir().mkpath(work);
				const QString zipPath = QDir(work).filePath(QStringLiteral("update.zip"));
				const QString extractDir = QDir(work).filePath(QStringLiteral("extract"));
				QDir().mkpath(extractDir);
				{
					QFile zf(zipPath);
					if (!zf.open(QIODevice::WriteOnly | QIODevice::Truncate) || zf.write(zip) != zip.size()) {
						closeProgress();
						QMessageBox::warning(dlgParent, QStringLiteral("LiveDelay"), T("Update.ApplyFailed").arg(QStringLiteral("zip_write")));
						return;
					}
				}

				// Windows tar extracts the release zip (forward-slash paths).
				QProcess tar;
				tar.setProgram(QStringLiteral("tar"));
				tar.setArguments({QStringLiteral("-xf"), zipPath, QStringLiteral("-C"), extractDir});
				tar.start();
				if (!tar.waitForFinished(60000) || tar.exitCode() != 0) {
					closeProgress();
					QMessageBox::warning(dlgParent, QStringLiteral("LiveDelay"), T("Update.ApplyFailed").arg(QStringLiteral("extract")));
					return;
				}

				const QString script = QDir(work).filePath(QStringLiteral("apply.ps1"));
				QString werr;
				if (!WriteUpdaterScript(script, pid, obsExeCopy, extractDir, dllDestCopy, dataDestCopy, &werr)) {
					closeProgress();
					QMessageBox::warning(dlgParent, QStringLiteral("LiveDelay"), T("Update.ApplyFailed").arg(werr));
					return;
				}

				const bool started = QProcess::startDetached(
					QStringLiteral("powershell.exe"),
					{QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
					 QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"), QStringLiteral("-File"), script});
				if (!started) {
					closeProgress();
					QMessageBox::warning(dlgParent, QStringLiteral("LiveDelay"), T("Update.ApplyFailed").arg(QStringLiteral("updater")));
					return;
				}

				closeProgress();
				// Quit OBS so the DLL unlocks; updater waits, copies, restarts.
				if (QWidget *main = static_cast<QWidget *>(obs_frontend_get_main_window()))
					QMetaObject::invokeMethod(main, "close", Qt::QueuedConnection);
			},
			Qt::QueuedConnection);
	});
	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	thread->start();
}

void UpdateChecker::Consider(UpdateInfo info, bool force)
{
	info.version = StripV(info.version);
	if (info.version.isEmpty())
		return;
	if (!NewerThan(info.version, CurrentVersion())) {
		emit upToDate();
		return;
	}
	if (!force && IsSkipped(info.version))
		return;
	emit found(info);
}

void UpdateChecker::ParseMirror(const QByteArray &body, bool force)
{
	const QJsonObject o = QJsonDocument::fromJson(body).object();
	UpdateInfo info;
	info.version = o.value("version").toString();
	info.tag = o.value("tag").toString();
	if (info.tag.isEmpty() && !info.version.isEmpty())
		info.tag = "v" + StripV(info.version);
	info.releaseUrl = o.value("url").toString();
	info.zipUrl = o.value("zip").toString();
	info.notes = o.value("notes").toString();
	if (info.releaseUrl.isEmpty())
		info.releaseUrl = "https://github.com/skullboypl/livedelay-obs/releases/latest";
	Consider(info, force);
}

void UpdateChecker::ParseGithub(const QByteArray &body, bool force)
{
	const QJsonObject o = QJsonDocument::fromJson(body).object();
	if (o.value("draft").toBool() || o.value("prerelease").toBool()) {
		emit upToDate();
		return;
	}
	UpdateInfo info;
	info.tag = o.value("tag_name").toString();
	info.version = StripV(info.tag);
	info.releaseUrl = o.value("html_url").toString();
	info.notes = o.value("body").toString().left(400);
	for (const QJsonValue &v : o.value("assets").toArray()) {
		const QJsonObject a = v.toObject();
		const QString name = a.value("name").toString();
		if (name.endsWith(".zip", Qt::CaseInsensitive) && !name.contains("sha256")) {
			info.zipUrl = a.value("browser_download_url").toString();
			break;
		}
	}
	Consider(info, force);
}

void UpdateChecker::Check(bool force)
{
	if (busy_)
		return;
	busy_ = true;

	// Background thread: WinHTTP is sync; keep the OBS UI responsive.
	QPointer<UpdateChecker> self(this);
	QThread *thread = QThread::create([self, force] {
		QByteArray body;
		QString err;
		bool usedMirror = false;
		if (HttpGet(QString::fromUtf8(kMirrorJson), nullptr, &body, &err)) {
			const QJsonObject o = QJsonDocument::fromJson(body).object();
			if (!o.value("version").toString().trimmed().isEmpty())
				usedMirror = true;
		}
		if (!usedMirror) {
			body.clear();
			err.clear();
			const wchar_t *hdr = L"Accept: application/vnd.github+json\r\n";
			if (!HttpGet(QString::fromUtf8(kGithubLatest), hdr, &body, &err)) {
				QMetaObject::invokeMethod(
					self, [self, err] {
						if (!self)
							return;
						self->busy_ = false;
						emit self->failed(err.isEmpty() ? QStringLiteral("network") : err);
					},
					Qt::QueuedConnection);
				return;
			}
			QMetaObject::invokeMethod(
				self,
				[self, force, body] {
					if (!self)
						return;
					self->ParseGithub(body, force);
					self->busy_ = false;
				},
				Qt::QueuedConnection);
			return;
		}
		QMetaObject::invokeMethod(
			self,
			[self, force, body] {
				if (!self)
					return;
				self->ParseMirror(body, force);
				self->busy_ = false;
			},
			Qt::QueuedConnection);
	});
	connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	thread->start();
}

void AttachUpdateBanner(QWidget *dockParent, QVBoxLayout *layout)
{
	auto *banner = new QFrame(dockParent);
	banner->setObjectName("ldUpdate");
	banner->setStyleSheet("#ldUpdate { background: rgba(90, 200, 250, .14); border: 1px solid rgba(90, 200, 250, .5); border-radius: 6px; }");
	auto *text = new QLabel(banner);
	text->setWordWrap(true);
	auto *get = new QPushButton(T("Update.UpdateNow"), banner);
	get->setProperty("ldText", "Update.UpdateNow");
	get->setCursor(Qt::PointingHandCursor);
	get->setStyleSheet("QPushButton { font-weight: 600; }");
	auto *later = new QPushButton(T("Update.Later"), banner);
	later->setProperty("ldText", "Update.Later");
	later->setCursor(Qt::PointingHandCursor);
	auto *row = new QHBoxLayout(banner);
	row->setContentsMargins(8, 6, 8, 6);
	row->addWidget(text, 1);
	row->addWidget(get);
	row->addWidget(later);
	banner->hide();
	layout->insertWidget(0, banner);

	auto *checker = new UpdateChecker(dockParent);
	QObject::connect(checker, &UpdateChecker::found, dockParent, [banner, text, get, later, dockParent](UpdateInfo info) {
		text->setText(T("Update.Banner").arg(info.version));
		banner->show();
		get->disconnect();
		later->disconnect();
		QObject::connect(get, &QPushButton::clicked, banner, [info, dockParent] {
			UpdateChecker::ApplyUpdate(info, dockParent);
		});
		QObject::connect(later, &QPushButton::clicked, banner, [banner, info] {
			UpdateChecker::SkipVersion(info.version);
			banner->hide();
		});
	});

	QTimer::singleShot(4000, checker, [checker] { checker->Check(false); });
	auto *timer = new QTimer(dockParent);
	timer->setInterval(12 * 60 * 60 * 1000);
	QObject::connect(timer, &QTimer::timeout, checker, [checker] { checker->Check(false); });
	timer->start();

	banner->setProperty("ldUpdateChecker", QVariant::fromValue(static_cast<QObject *>(checker)));
}

} // namespace livedelay
