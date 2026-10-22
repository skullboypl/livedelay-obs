#include "update.hpp"
#include "i18n.hpp"
#include "version.hpp"

Q_DECLARE_METATYPE(livedelay::UpdateInfo)

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
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
#include <QProgressBar>
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

#include <memory>
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
			const QString &dllDestDir, const QString &dataDestDir, const QString &version, QString *err)
{
	// Zip has obs-plugins\64bit\*.dll and data\obs-plugins\livedelay-obs\*
	const QString dllSrc = QDir(extractDir).filePath(QStringLiteral("obs-plugins/64bit/livedelay-obs.dll"));
	const QString dataSrc = QDir(extractDir).filePath(QStringLiteral("data/obs-plugins/livedelay-obs"));
	const QString logPath = QDir::temp().filePath(QStringLiteral("livedelay-update.log"));
	const QString primaryDll = QDir::toNativeSeparators(QDir(dllDestDir).filePath(QStringLiteral("livedelay-obs.dll")));

	// When loaded from ProgramData\...\bin\64bit\, also refresh the flat DLL (OBS 33+).
	QString flatDll;
	{
		const QString native = QDir::toNativeSeparators(dllDestDir);
		if (native.contains(QStringLiteral("\\plugins\\livedelay-obs\\bin\\64bit"), Qt::CaseInsensitive)) {
			QDir bin64(dllDestDir);
			bin64.cdUp(); // bin
			bin64.cdUp(); // livedelay-obs
			flatDll = QDir::toNativeSeparators(bin64.filePath(QStringLiteral("livedelay-obs.dll")));
		}
	}

	// Localized UI strings for the WinForms window that stays up after OBS quits.
	const QString uiTitle = T("Update.ProgressTitle");
	const QString uiVer = T("Update.ProgressVersion").arg(version);
	const QString uiWait = T("Update.ProgressWaiting");
	const QString uiCopy = T("Update.ProgressCopying");
	const QString uiData = T("Update.ProgressData");
	const QString uiStart = T("Update.ProgressRestarting");
	const QString uiDone = T("Update.ProgressDone");
	const QString uiFail = T("Update.ProgressFailed");

	QString ps;
	ps += QStringLiteral("$ErrorActionPreference='Continue'\n");
	ps += QStringLiteral("Start-Transcript -Path %1 -Force | Out-Null\n").arg(PsQuote(logPath));
	ps += QStringLiteral("$obsPidWait=%1\n").arg(obsPid);
	ps += QStringLiteral("$obsExe=%1\n").arg(PsQuote(QDir::toNativeSeparators(obsExe)));
	ps += QStringLiteral("$dllSrc=%1\n").arg(PsQuote(QDir::toNativeSeparators(dllSrc)));
	ps += QStringLiteral("$dllDst=%1\n").arg(PsQuote(primaryDll));
	ps += QStringLiteral("$dllFlat=%1\n").arg(PsQuote(flatDll));
	ps += QStringLiteral("$dataSrc=%1\n").arg(PsQuote(QDir::toNativeSeparators(dataSrc)));
	ps += QStringLiteral("$dataDst=%1\n").arg(PsQuote(QDir::toNativeSeparators(dataDestDir)));
	ps += QStringLiteral("$uiTitle=%1\n").arg(PsQuote(uiTitle));
	ps += QStringLiteral("$uiVer=%1\n").arg(PsQuote(uiVer));
	ps += QStringLiteral("$uiWait=%1\n").arg(PsQuote(uiWait));
	ps += QStringLiteral("$uiCopy=%1\n").arg(PsQuote(uiCopy));
	ps += QStringLiteral("$uiData=%1\n").arg(PsQuote(uiData));
	ps += QStringLiteral("$uiStart=%1\n").arg(PsQuote(uiStart));
	ps += QStringLiteral("$uiDone=%1\n").arg(PsQuote(uiDone));
	ps += QStringLiteral("$uiFail=%1\n").arg(PsQuote(uiFail));
	// WinForms progress UI (STA + hidden console). Survives OBS exit.
	ps += QStringLiteral(
		"Add-Type -AssemblyName System.Windows.Forms\n"
		"Add-Type -AssemblyName System.Drawing\n"
		"try {\n"
		"  Add-Type -Name W -Namespace LdHide -MemberDefinition @'\n"
		"[DllImport(\"user32.dll\")] public static extern bool ShowWindow(IntPtr h,int c);\n"
		"[DllImport(\"kernel32.dll\")] public static extern IntPtr GetConsoleWindow();\n"
		"'@\n"
		"  $cw=[LdHide.W]::GetConsoleWindow()\n"
		"  if ($cw -ne [IntPtr]::Zero) { [void][LdHide.W]::ShowWindow($cw,0) }\n"
		"} catch {}\n"
		"$form=New-Object System.Windows.Forms.Form\n"
		"$form.Text=$uiTitle\n"
		"$form.ClientSize=New-Object System.Drawing.Size(440,158)\n"
		"$form.StartPosition='CenterScreen'\n"
		"$form.FormBorderStyle='FixedDialog'\n"
		"$form.MaximizeBox=$false; $form.MinimizeBox=$false; $form.ControlBox=$false\n"
		"$form.TopMost=$true\n"
		"$form.BackColor=[System.Drawing.Color]::FromArgb(22,26,34)\n"
		"$brand=New-Object System.Windows.Forms.Label\n"
		"$brand.Text='LiveDelay'; $brand.AutoSize=$true\n"
		"$brand.Font=New-Object System.Drawing.Font('Segoe UI Semibold',13)\n"
		"$brand.ForeColor=[System.Drawing.Color]::FromArgb(61,220,132)\n"
		"$brand.Location=New-Object System.Drawing.Point(22,14)\n"
		"$verLbl=New-Object System.Windows.Forms.Label\n"
		"$verLbl.Text=$uiVer; $verLbl.AutoSize=$true\n"
		"$verLbl.Font=New-Object System.Drawing.Font('Segoe UI',9)\n"
		"$verLbl.ForeColor=[System.Drawing.Color]::FromArgb(160,172,190)\n"
		"$verLbl.Location=New-Object System.Drawing.Point(22,42)\n"
		"$status=New-Object System.Windows.Forms.Label\n"
		"$status.Text=$uiWait\n"
		"$status.Font=New-Object System.Drawing.Font('Segoe UI',9.75)\n"
		"$status.ForeColor=[System.Drawing.Color]::White\n"
		"$status.Location=New-Object System.Drawing.Point(22,72)\n"
		"$status.Size=New-Object System.Drawing.Size(396,24)\n"
		"$bar=New-Object System.Windows.Forms.ProgressBar\n"
		"$bar.Location=New-Object System.Drawing.Point(22,106)\n"
		"$bar.Size=New-Object System.Drawing.Size(396,24)\n"
		"$bar.Minimum=0; $bar.Maximum=100; $bar.Value=4; $bar.Style='Continuous'\n"
		"$form.Controls.AddRange(@($brand,$verLbl,$status,$bar))\n"
		"$form.Show(); $form.Activate(); $form.Refresh()\n"
		"[void][System.Windows.Forms.Application]::DoEvents()\n"
		"function Set-Step([string]$text,[int]$pct) {\n"
		"  if ($pct -lt 0) { $pct=0 }; if ($pct -gt 100) { $pct=100 }\n"
		"  $status.Text=$text; $bar.Value=$pct\n"
		"  $form.Refresh(); [void][System.Windows.Forms.Application]::DoEvents()\n"
		"}\n"
		"function Test-ObsAlive {\n"
		"  if ($obsPidWait -gt 0) {\n"
		"    $p=Get-Process -Id $obsPidWait -ErrorAction SilentlyContinue\n"
		"    if ($p) { return $true }\n"
		"  }\n"
		"  return [bool](Get-Process obs64,obs32 -ErrorAction SilentlyContinue)\n"
		"}\n"
		"try {\n"
		"  Set-Step $uiWait 8\n"
		"  $deadline=(Get-Date).AddMinutes(3)\n"
		"  $pulse=8\n"
		"  while ((Get-Date) -lt $deadline) {\n"
		"    if (-not (Test-ObsAlive)) { break }\n"
		"    $pulse=[Math]::Min(38,$pulse+1)\n"
		"    Set-Step $uiWait $pulse\n"
		"    Start-Sleep -Milliseconds 320\n"
		"  }\n"
		"  if (Test-ObsAlive) {\n"
		"    Set-Step $uiWait 40\n"
		"    Get-Process obs64,obs32 -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue\n"
		"    Start-Sleep -Seconds 2\n"
		"  }\n"
		"  Start-Sleep -Milliseconds 800\n"
		"  $ErrorActionPreference='Stop'\n"
		"  if (-not (Test-Path -LiteralPath $dllSrc)) { throw \"missing $dllSrc\" }\n"
		"  Set-Step $uiCopy 55\n"
		"  $targets=New-Object System.Collections.Generic.List[string]\n"
		"  [void]$targets.Add($dllDst)\n"
		"  if ($dllFlat) { [void]$targets.Add($dllFlat) }\n"
		"  foreach ($dst in $targets) {\n"
		"    $dllDir=Split-Path -Parent $dst\n"
		"    New-Item -ItemType Directory -Force -Path $dllDir | Out-Null\n"
		"    Copy-Item -LiteralPath $dllSrc -Destination $dst -Force\n"
		"    try { Unblock-File -LiteralPath $dst -ErrorAction SilentlyContinue } catch {}\n"
		"  }\n"
		"  Set-Step $uiData 78\n"
		"  New-Item -ItemType Directory -Force -Path $dataDst | Out-Null\n"
		"  Get-ChildItem -LiteralPath $dataSrc -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dataDst -Recurse -Force }\n"
		"  Set-Step $uiStart 92\n"
		"  Start-Sleep -Milliseconds 400\n"
		"  if (Test-Path -LiteralPath $obsExe) {\n"
		"    $wd=Split-Path -Parent $obsExe\n"
		"    Start-Process -FilePath $obsExe -WorkingDirectory $wd\n"
		"  }\n"
		"  Set-Step $uiDone 100\n"
		"  Start-Sleep -Milliseconds 900\n"
		"} catch {\n"
		"  $msg=$uiFail + [string]$_.Exception.Message\n"
		"  Set-Step $msg 100\n"
		"  $form.ControlBox=$true\n"
		"  [void][System.Windows.Forms.MessageBox]::Show($form,$msg,$uiTitle,'OK','Error')\n"
		"} finally {\n"
		"  try { $form.Close(); $form.Dispose() } catch {}\n"
		"  try { Stop-Transcript | Out-Null } catch {}\n"
		"}\n");

	QFile f(scriptPath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		*err = QStringLiteral("script_write");
		return false;
	}
	// UTF-8 BOM helps Windows PowerShell 5.1 -File parse reliably.
	static const char bom[] = "\xEF\xBB\xBF";
	f.write(bom, 3);
	f.write(ps.toUtf8());
	f.close();
	return true;
}

bool OutputsBusy()
{
	return obs_frontend_streaming_active() || obs_frontend_recording_active() || obs_frontend_replay_buffer_active() ||
	       obs_frontend_virtualcam_active();
}

void StopOutputs()
{
	if (obs_frontend_streaming_active())
		obs_frontend_streaming_stop();
	if (obs_frontend_recording_active())
		obs_frontend_recording_stop();
	if (obs_frontend_replay_buffer_active())
		obs_frontend_replay_buffer_stop();
	if (obs_frontend_virtualcam_active())
		obs_frontend_stop_virtualcam();
}

QDialog *MakeUpdateProgressDialog(QWidget *parent, const QString &version, QLabel **statusOut, QProgressBar **barOut)
{
	auto *dlg = new QDialog(parent);
	dlg->setWindowTitle(T("Update.ProgressTitle"));
	dlg->setModal(true);
	dlg->setWindowFlags((dlg->windowFlags() | Qt::CustomizeWindowHint | Qt::WindowTitleHint) & ~Qt::WindowCloseButtonHint);
	dlg->setFixedSize(400, 130);
	auto *lay = new QVBoxLayout(dlg);
	lay->setContentsMargins(18, 16, 18, 16);
	lay->setSpacing(10);
	auto *title = new QLabel(T("Update.ProgressVersion").arg(version));
	title->setStyleSheet(QStringLiteral("font-weight:600; font-size:13px;"));
	auto *status = new QLabel(T("Update.Downloading").arg(version));
	status->setWordWrap(true);
	auto *bar = new QProgressBar;
	bar->setRange(0, 0); // indeterminate while downloading
	bar->setTextVisible(false);
	bar->setFixedHeight(18);
	lay->addWidget(title);
	lay->addWidget(status);
	lay->addWidget(bar);
	if (statusOut)
		*statusOut = status;
	if (barOut)
		*barOut = bar;
	return dlg;
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

	QLabel *progressStatus = nullptr;
	QProgressBar *progressBar = nullptr;
	auto *progress = MakeUpdateProgressDialog(parent, info.version, &progressStatus, &progressBar);
	progress->show();
	progress->raise();

	const UpdateInfo infoCopy = info;
	const QString dllDestCopy = dllDest;
	const QString dataDestCopy = dataDest;
	const QString obsExeCopy = obsExe;
	const DWORD pid = GetCurrentProcessId();
	QPointer<QWidget> parentGuard(parent);
	QPointer<QDialog> progressGuard(progress);
	QPointer<QLabel> statusGuard(progressStatus);
	QPointer<QProgressBar> barGuard(progressBar);

	QThread *thread = QThread::create([infoCopy, dllDestCopy, dataDestCopy, obsExeCopy, pid, parentGuard, progressGuard, statusGuard,
					  barGuard] {
		QByteArray zip;
		QString netErr;
		const bool ok = HttpGet(infoCopy.zipUrl, nullptr, &zip, &netErr);
		QMetaObject::invokeMethod(
			qApp,
			[ok, zip, netErr, infoCopy, dllDestCopy, dataDestCopy, obsExeCopy, pid, parentGuard, progressGuard, statusGuard,
			 barGuard] {
				auto setStatus = [&](const QString &text, int determinatePct) {
					if (statusGuard)
						statusGuard->setText(text);
					if (barGuard) {
						if (determinatePct < 0) {
							barGuard->setRange(0, 0);
						} else {
							barGuard->setRange(0, 100);
							barGuard->setValue(determinatePct);
						}
					}
				};
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

				setStatus(T("Update.ProgressExtracting"), 55);

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

				setStatus(T("Update.ProgressLaunching"), 85);

				const QString script = QDir(work).filePath(QStringLiteral("apply.ps1"));
				QString werr;
				if (!WriteUpdaterScript(script, pid, obsExeCopy, extractDir, dllDestCopy, dataDestCopy, infoCopy.version, &werr)) {
					closeProgress();
					QMessageBox::warning(dlgParent, QStringLiteral("LiveDelay"), T("Update.ApplyFailed").arg(werr));
					return;
				}

				// Hidden console; WinForms UI in apply.ps1 shows progress after OBS quits.
				const bool started = QProcess::startDetached(
					QStringLiteral("powershell.exe"),
					{QStringLiteral("-NoProfile"), QStringLiteral("-STA"), QStringLiteral("-WindowStyle"),
					 QStringLiteral("Hidden"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
					 QStringLiteral("-File"), script});
				if (!started) {
					closeProgress();
					QMessageBox::warning(dlgParent, QStringLiteral("LiveDelay"), T("Update.ApplyFailed").arg(QStringLiteral("updater")));
					return;
				}

				setStatus(T("Update.ProgressWaiting"), 95);
				// Brief beat so the user sees the hand-off, then quit OBS; WinForms UI continues.
				QTimer::singleShot(450, qApp, [progressGuard]() {
					if (progressGuard) {
						progressGuard->close();
						progressGuard->deleteLater();
					}
					if (QWidget *main = static_cast<QWidget *>(obs_frontend_get_main_window()))
						QMetaObject::invokeMethod(main, "close", Qt::QueuedConnection);
				});
			},
			Qt::QueuedConnection);
	});
	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);

	// OBS refuses to quit while a stream or recording is running, so stop those first.
	auto startDownload = [thread] { thread->start(); };
	if (!OutputsBusy()) {
		startDownload();
		return;
	}
	if (progressStatus)
		progressStatus->setText(T("Update.ProgressStopping"));
	StopOutputs();
	auto *wait = new QTimer(qApp);
	auto left = std::make_shared<int>(75); // 75 × 200 ms = 15 s
	const QString version = info.version;
	QObject::connect(wait, &QTimer::timeout, qApp, [wait, left, startDownload, statusGuard, version] {
		if (OutputsBusy() && *left > 0) {
			--*left;
			StopOutputs();
			return;
		}
		wait->stop();
		wait->deleteLater();
		if (statusGuard)
			statusGuard->setText(T("Update.Downloading").arg(version));
		startDownload();
	});
	wait->start(200);
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
