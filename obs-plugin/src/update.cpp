#include "update.hpp"
#include "i18n.hpp"
#include "version.hpp"

Q_DECLARE_METATYPE(livedelay::UpdateInfo)

#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <vector>

namespace livedelay {
namespace {

constexpr const char *kGithubLatest = "https://api.github.com/repos/skullboypl/livedelay-obs/releases/latest";
constexpr const char *kMirrorJson = "https://livedelay.vxh.pl/version.json";
constexpr const char *kUserAgent = "livedelay-obs/" LD_VERSION_STRING " (+https://livedelay.vxh.pl)";
constexpr const char *kSkipKey = "update/skipVersion";

QNetworkAccessManager *Nam()
{
	static QNetworkAccessManager *n = new QNetworkAccessManager;
	return n;
}

// Returns true if a.b.c is strictly greater than x.y.z.
bool NewerThan(const QString &remote, const QString &local)
{
	auto parts = [](QString v) {
		v.remove(QChar('v'), Qt::CaseInsensitive);
		const QStringList s = v.split('.');
		std::vector<int> out;
		for (int i = 0; i < 3; i++)
			out.push_back(i < s.size() ? s[i].toInt() : 0);
		return out;
	};
	const auto a = parts(remote), b = parts(local);
	for (int i = 0; i < 3; i++) {
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

	auto finish = [this] { busy_ = false; };

	// 1) Fast mirror on the product site (updated with each release).
	QNetworkRequest mirror(QUrl(QString::fromUtf8(kMirrorJson)));
	mirror.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
	mirror.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	QNetworkReply *m = Nam()->get(mirror);
	auto fetchGithub = [this, force, finish] {
		QNetworkRequest req(QUrl(QString::fromUtf8(kGithubLatest)));
		req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
		req.setRawHeader("Accept", "application/vnd.github+json");
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
		QNetworkReply *r = Nam()->get(req);
		connect(r, &QNetworkReply::finished, this, [this, r, force, finish] {
			if (r->error() != QNetworkReply::NoError) {
				emit failed(r->errorString());
				r->deleteLater();
				finish();
				return;
			}
			ParseGithub(r->readAll(), force);
			r->deleteLater();
			finish();
		});
	};

	connect(m, &QNetworkReply::finished, this, [this, m, force, finish, fetchGithub] {
		const bool ok = m->error() == QNetworkReply::NoError;
		const QByteArray body = m->readAll();
		m->deleteLater();
		if (ok && !body.isEmpty()) {
			const QJsonObject o = QJsonDocument::fromJson(body).object();
			if (!o.value("version").toString().trimmed().isEmpty()) {
				ParseMirror(body, force);
				finish();
				return;
			}
		}
		fetchGithub();
	});
}

void AttachUpdateBanner(QWidget *dockParent, QVBoxLayout *layout)
{
	auto *banner = new QFrame(dockParent);
	banner->setObjectName("ldUpdate");
	banner->setStyleSheet("#ldUpdate { background: rgba(90, 200, 250, .14); border: 1px solid rgba(90, 200, 250, .5); border-radius: 6px; }");
	auto *text = new QLabel(banner);
	text->setWordWrap(true);
	auto *get = new QPushButton(T("Update.Download"), banner);
	get->setProperty("ldText", "Update.Download");
	get->setCursor(Qt::PointingHandCursor);
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
	QObject::connect(checker, &UpdateChecker::found, dockParent, [banner, text, get, later](UpdateInfo info) {
		text->setText(T("Update.Available").arg(info.version).arg(UpdateChecker::CurrentVersion()));
		banner->show();
		get->disconnect();
		later->disconnect();
		QObject::connect(get, &QPushButton::clicked, banner, [info] { UpdateChecker::OpenDownload(info); });
		QObject::connect(later, &QPushButton::clicked, banner, [banner, info] {
			UpdateChecker::SkipVersion(info.version);
			banner->hide();
		});
	});

	// First check a few seconds after the dock opens, then every 12 hours.
	QTimer::singleShot(4000, checker, [checker] { checker->Check(false); });
	auto *timer = new QTimer(dockParent);
	timer->setInterval(12 * 60 * 60 * 1000);
	QObject::connect(timer, &QTimer::timeout, checker, [checker] { checker->Check(false); });
	timer->start();

	banner->setProperty("ldUpdateChecker", QVariant::fromValue(static_cast<QObject *>(checker)));
}

} // namespace livedelay
