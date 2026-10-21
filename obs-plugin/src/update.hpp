#pragma once

#include <QObject>
#include <QString>

class QWidget;
class QVBoxLayout;

namespace livedelay {

// Result of a successful "newer release found" check.
struct UpdateInfo {
	QString version; // e.g. "0.2.1" (no leading v)
	QString tag; // e.g. "v0.2.1"
	QString releaseUrl; // html page
	QString zipUrl; // direct .zip asset when present
	QString notes; // truncated body
};

// Checks GitHub Releases (and livedelay.vxh.pl/version.json as a fast mirror).
// Emits found() only when the remote version is newer than LD_VERSION_STRING
// and the user has not dismissed that version for this session/profile.
class UpdateChecker : public QObject {
	Q_OBJECT
public:
	explicit UpdateChecker(QObject *parent = nullptr);

	void Check(bool force = false); // force = ignore "skip this version"
	static QString CurrentVersion();
	static void SkipVersion(const QString &version);
	static bool IsSkipped(const QString &version);
	/// Fallback: open the zip URL in the browser.
	static void OpenDownload(const UpdateInfo &info);
	/// One-click: download zip → quit OBS → replace plugin files → restart OBS.
	/// parent is used for confirm/progress dialogs (dock or settings).
	static void ApplyUpdate(const UpdateInfo &info, QWidget *parent);

signals:
	void found(livedelay::UpdateInfo info);
	void upToDate();
	void failed(QString reason);

private:
	void ParseGithub(const QByteArray &body, bool force);
	void ParseMirror(const QByteArray &body, bool force);
	void Consider(UpdateInfo info, bool force);
	bool busy_ = false;
};

// Blue banner shown in the dock when an update is available.
void AttachUpdateBanner(QWidget *dockParent, QVBoxLayout *layout);

} // namespace livedelay
