#include "log.hpp"

#include "client.hpp"
#include "i18n.hpp"
#include "settings.hpp"
#include "version.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/base.h>

#include <QClipboard>
#include <QDateTime>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace livedelay {
namespace {

struct Line {
	qint64 ms = 0;
	int level = LOG_INFO;
	std::string text;
};

std::mutex g_mu;
std::deque<Line> g_lines;
constexpr size_t kMaxLines = 400;

log_handler_t g_prev = nullptr;
void *g_prevParam = nullptr;
bool g_installed = false;

const char *LevelName(int level)
{
	if (level <= LOG_ERROR)
		return "ERROR";
	if (level <= LOG_WARNING)
		return "WARN";
	if (level <= LOG_INFO)
		return "INFO";
	return "DEBUG";
}

void OnLog(int level, const char *format, va_list args, void *)
{
	char buf[4096];
	va_list copy;
	va_copy(copy, args);
	std::vsnprintf(buf, sizeof(buf), format ? format : "", copy);
	va_end(copy);
	if (g_prev)
		g_prev(level, format, args, g_prevParam);
	if (!std::strstr(buf, "[livedelay]"))
		return;
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	std::lock_guard<std::mutex> lock(g_mu);
	g_lines.push_back(Line{ms, level, buf});
	while (g_lines.size() > kMaxLines)
		g_lines.pop_front();
}

QString Redact(QString text)
{
	const Settings settings = Settings::Load(ConfigPath());
	for (const Destination &dest : settings.dests) {
		if (dest.key.size() < 8)
			continue;
		text.replace(QString::fromStdString(dest.key), QStringLiteral("••••"));
	}
	return text;
}

QString Report()
{
	QString out;
	out += QStringLiteral("LiveDelay %1\n").arg(QStringLiteral(LD_VERSION_STRING));

	obs_output_t *streaming = obs_frontend_get_streaming_output();
	if (streaming) {
		out += obs_output_active(streaming) ? T("Logs.Streaming") : T("Logs.Idle");
		const char *err = obs_output_get_last_error(streaming);
		if (err && *err)
			out += QStringLiteral("\nOBS: ") + QString::fromUtf8(err);
		out += QLatin1Char('\n');
		obs_output_release(streaming);
	} else {
		out += T("Logs.Idle") + QLatin1Char('\n');
	}

	int status = 0;
	const std::string body = Request("GET", "/state", &status);
	const QJsonObject state = QJsonDocument::fromJson(QByteArray::fromStdString(body)).object();
	if (status != 200 || state.isEmpty()) {
		out += T("Logs.ProxyDown") + QLatin1Char('\n');
	} else {
		out += QStringLiteral("proxy  %1  delay %2 s  buffer %3 s  in %4 kb/s\n")
			       .arg(state.value("mode").toString("?"),
				    QString::number(state.value("delay").toDouble(), 'f', 1),
				    QString::number(state.value("buffer_s").toDouble(), 'f', 1),
				    QString::number(state.value("in_kbps").toDouble(), 'f', 0));
		if (obs_frontend_streaming_active() && !state.value("input").toBool())
			out += T("Logs.NoSignal") + QLatin1Char('\n');
		const QJsonArray dests = state.value("dests").toArray();
		if (dests.isEmpty())
			out += T("Logs.NoPlatforms") + QLatin1Char('\n');
		for (const QJsonValue &value : dests) {
			const QJsonObject dest = value.toObject();
			const QString error = dest.value("error").toString();
			out += QStringLiteral("  %1  %2  %3 kb/s%4\n")
				       .arg(dest.value("name").toString(), dest.value("state").toString(),
					    QString::number(dest.value("kbps").toDouble(), 'f', 0),
					    error.isEmpty() ? QString() : QStringLiteral("  ") + error);
		}
	}
	out += QStringLiteral("----\n");

	std::deque<Line> lines;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		lines = g_lines;
	}
	if (lines.empty()) {
		out += T("Logs.Empty") + QLatin1Char('\n');
	} else {
		for (const Line &line : lines) {
			const QString when = QDateTime::fromMSecsSinceEpoch(line.ms).toString(QStringLiteral("HH:mm:ss"));
			out += when + QStringLiteral("  ") + QLatin1String(LevelName(line.level)) + QStringLiteral("  ") +
			       QString::fromUtf8(line.text.c_str()) + QLatin1Char('\n');
		}
	}
	return Redact(out);
}

class LogPage : public QWidget {
public:
	explicit LogPage(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *hint = new QLabel(T("Logs.Hint"));
		hint->setWordWrap(true);
		hint->setTextFormat(Qt::RichText);
		hint->setOpenExternalLinks(true);
		hint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));

		view_ = new QPlainTextEdit;
		view_->setReadOnly(true);
		view_->setLineWrapMode(QPlainTextEdit::NoWrap);
		view_->setUndoRedoEnabled(false);
		QFont font(QStringLiteral("Consolas"));
		font.setStyleHint(QFont::Monospace);
		font.setPointSize(9);
		view_->setFont(font);

		auto *copy = new QPushButton(T("Logs.Copy"));
		auto *refresh = new QPushButton(T("Logs.Refresh"));
		auto *clear = new QPushButton(T("Logs.Clear"));
		for (QPushButton *button : {copy, refresh, clear}) {
			button->ensurePolished();
			button->setMinimumWidth(button->sizeHint().width());
			button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
		}

		auto *row = new QHBoxLayout;
		row->setContentsMargins(0, 0, 0, 0);
		row->addWidget(copy);
		row->addWidget(refresh);
		row->addWidget(clear);
		row->addStretch();

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(4, 8, 4, 4);
		layout->addWidget(hint);
		layout->addWidget(view_, 1);
		layout->addLayout(row);

		connect(copy, &QPushButton::clicked, this, [this, copy] {
			QGuiApplication::clipboard()->setText(view_->toPlainText());
			const QString previous = copy->text();
			copy->setText(T("Logs.Copied"));
			QTimer::singleShot(1200, copy, [copy, previous] { copy->setText(previous); });
		});
		connect(refresh, &QPushButton::clicked, this, [this] { Refresh(true); });
		connect(clear, &QPushButton::clicked, this, [this] {
			{
				std::lock_guard<std::mutex> lock(g_mu);
				g_lines.clear();
			}
			Refresh(true);
		});

		auto *timer = new QTimer(this);
		timer->setInterval(1000);
		connect(timer, &QTimer::timeout, this, [this] {
			if (isVisible() && !view_->textCursor().hasSelection())
				Refresh(false);
		});
		timer->start();
		Refresh(true);
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		Refresh(true);
	}

private:
	void Refresh(bool follow)
	{
		auto *bar = view_->verticalScrollBar();
		const bool stick = follow || bar->value() >= bar->maximum() - 8;
		const int pos = bar->value();
		view_->setPlainText(Report());
		bar->setValue(stick ? bar->maximum() : pos);
	}

	QPlainTextEdit *view_ = nullptr;
};

} // namespace

void InstallLogCapture()
{
	if (g_installed)
		return;
	base_get_log_handler(&g_prev, &g_prevParam);
	if (g_prev == OnLog)
		return;
	base_set_log_handler(OnLog, nullptr);
	g_installed = true;
}

void UninstallLogCapture()
{
	if (!g_installed)
		return;
	base_set_log_handler(g_prev, g_prevParam);
	g_prev = nullptr;
	g_prevParam = nullptr;
	g_installed = false;
}

QWidget *MakeLogPage()
{
	return new LogPage;
}

} // namespace livedelay
