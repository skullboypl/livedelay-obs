#include "dock.hpp"
#include "client.hpp"
#include "i18n.hpp"
#include "icons.hpp"
#include "settings.hpp"

#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <chrono>
#include <thread>

namespace livedelay {

// A tiny bitrate line for one row of the panel.
class Sparkline : public QWidget {
public:
	Sparkline(const std::deque<double> &values, QColor color) : values_(values), color_(color)
	{
		setFixedSize(64, 18);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		if (values_.size() < 2)
			return;
		double max = 1000;
		for (double v : values_)
			max = std::max(max, v);
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		QPainterPath line;
		const double w = width() - 2, h = height() - 3;
		for (size_t i = 0; i < values_.size(); i++) {
			const double x = 1 + w * double(i + kHistory - values_.size()) / (kHistory - 1);
			const double y = 1.5 + h - values_[i] / (max * 1.1) * h;
			i ? line.lineTo(x, y) : line.moveTo(x, y);
		}
		p.setPen(QPen(color_, 1.6));
		p.drawPath(line);
	}

private:
	std::deque<double> values_;
	QColor color_;
};

// Buttons remember their text keys so Retranslate() can switch language live.
static QPushButton *Button(const char *text, const char *tip, Icon icon, const char *accent = nullptr)
{
	auto *b = new QPushButton(T(text));
	b->setToolTip(T(tip));
	b->setProperty("ldText", text);
	b->setProperty("ldTip", tip);
	b->setIcon(MakeIcon(icon, QColor(accent ? accent : "#c9ccd3")));
	b->setIconSize(QSize(16, 16));
	b->setMinimumHeight(32);
	b->setCursor(Qt::PointingHandCursor);
	if (accent) {
		// A tint of the action's colour that works on light and dark OBS themes.
		QColor c(accent);
		b->setStyleSheet(QString("QPushButton { background: rgba(%1,%2,%3,.14); border: 1px solid rgba(%1,%2,%3,.45);"
					 " border-radius: 6px; padding: 4px 10px; font-weight: 600; }"
					 "QPushButton:hover { background: rgba(%1,%2,%3,.26); }"
					 "QPushButton:pressed { background: rgba(%1,%2,%3,.36); }"
					 "QPushButton:disabled { background: transparent; border-color: rgba(128,132,142,.35); }")
					 .arg(c.red())
					 .arg(c.green())
					 .arg(c.blue()));
	}
	return b;
}

static QLabel *Label(const char *text)
{
	auto *l = new QLabel(T(text));
	l->setProperty("ldText", text);
	return l;
}

void Dock::Retranslate()
{
	for (QWidget *w : findChildren<QWidget *>()) {
		const QByteArray text = w->property("ldText").toByteArray();
		const QByteArray tip = w->property("ldTip").toByteArray();
		if (auto *b = qobject_cast<QPushButton *>(w); b && !text.isEmpty())
			b->setText(T(text.constData()));
		else if (auto *l = qobject_cast<QLabel *>(w); l && !text.isEmpty())
			l->setText(T(text.constData()));
		if (!tip.isEmpty())
			w->setToolTip(T(tip.constData()));
	}
	delay_->setToolTip(T("Dock.DelayTip"));
	if (last_.empty())
		ShowOffline();
	else
		ShowState(last_);
}

Dock::Dock(QWidget *parent) : QWidget(parent)
{
	// Shown while OBS streams somewhere else than LiveDelay.
	banner_ = new QFrame;
	banner_->setObjectName("ldBanner");
	banner_->setStyleSheet("#ldBanner { background: rgba(255, 191, 60, .14); border: 1px solid rgba(255, 191, 60, .45); border-radius: 6px; }");
	auto *bannerText = Label("Dock.NotRouted");
	bannerText->setWordWrap(true);
	auto *route = Button("Dock.Route", "Dock.Route", Icon::Play, "#ffbf3c");
	auto *bannerLayout = new QHBoxLayout(banner_);
	bannerLayout->addWidget(bannerText, 1);
	bannerLayout->addWidget(route);
	banner_->hide();

	status_ = new QLabel;
	status_->setAlignment(Qt::AlignCenter);
	status_->setMinimumHeight(36);
	detail_ = new QLabel;
	detail_->setWordWrap(true);
	detail_->setAlignment(Qt::AlignCenter);
	error_ = new QLabel;
	error_->setWordWrap(true);
	error_->setStyleSheet("color: #ff8a80;");
	error_->hide();

	delay_ = new QSpinBox;
	delay_->setRange(1, 3600);
	delay_->setValue(15);
	delay_->setSuffix(" s");
	delay_->setToolTip(T("Dock.DelayTip"));
	delay_->setMinimumHeight(32);
	arm_ = Button("Dock.Arm", "Dock.ArmTip", Icon::Arm, "#ffbf3c");
	activate_ = Button("Dock.Activate", "Dock.ActivateTip", Icon::Activate, "#ff5a4f");
	cut_ = Button("Dock.Cut", "Dock.CutTip", Icon::Live, "#3ddc84");
	cut_after_ = Button("Dock.CutAfter", "Dock.CutAfterTip", Icon::LiveAfter, "#3ddc84");
	disarm_ = Button("Dock.Disarm", "Dock.DisarmTip", Icon::Disarm);
	start_ = Button("Dock.Restart", "Dock.RestartTip", Icon::Restart, "#5ac8fa");
	auto *settings = Button("Dock.Settings", "Dock.SettingsTip", Icon::Settings);
	auto *preview = Button("Dock.Preview", "Dock.PreviewTip", Icon::Preview);
	auto *help = Button("Dock.Help", "Dock.HelpTip", Icon::Help);
	fill_ = new QProgressBar;
	fill_->setTextVisible(false);
	fill_->setMaximumHeight(6);
	fill_->hide();

	// Platforms with their own Start / Stop.
	dests_ = new QFrame;
	dests_->setObjectName("ldDests");
	dests_->setStyleSheet("#ldDests { border: 1px solid palette(mid); border-radius: 6px; }");
	destGrid_ = new QGridLayout(dests_);
	destGrid_->setContentsMargins(8, 6, 8, 6);
	destGrid_->setColumnStretch(1, 1);

	auto *armRow = new QHBoxLayout;
	armRow->addWidget(delay_);
	armRow->addWidget(arm_, 1);
	auto *mainRow = new QHBoxLayout;
	mainRow->addWidget(activate_);
	mainRow->addWidget(cut_);
	auto *moreRow = new QHBoxLayout;
	moreRow->addWidget(cut_after_);
	moreRow->addWidget(disarm_);
	auto *toolsRow = new QHBoxLayout;
	toolsRow->addWidget(settings);
	toolsRow->addWidget(preview);
	toolsRow->addWidget(help);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(banner_);
	layout->addWidget(status_);
	layout->addWidget(fill_);
	layout->addWidget(detail_);
	layout->addLayout(armRow);
	layout->addLayout(mainRow);
	layout->addLayout(moreRow);
	layout->addWidget(dests_);
	layout->addWidget(start_);
	layout->addLayout(toolsRow);
	layout->addWidget(error_);
	layout->addStretch();

	connect(arm_, &QPushButton::clicked, this, [this] { Hotkey("arm"); });
	connect(activate_, &QPushButton::clicked, this, [this] { Act("activate"); });
	connect(cut_, &QPushButton::clicked, this, [this] { Act("cut"); });
	connect(cut_after_, &QPushButton::clicked, this, [this] { Act("cut-after"); });
	connect(disarm_, &QPushButton::clicked, this, [this] { Act("disarm"); });
	connect(start_, &QPushButton::clicked, this, [this] { ShowError(QString::fromStdString(livedelay::StartProxy())); });
	connect(settings, &QPushButton::clicked, this, [this] { ShowSettings(window()); });
	connect(preview, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QString::fromStdString(LivePreviewUrl()))); });
	connect(help, &QPushButton::clicked, this, [this] { ShowHelp(window()); });
	connect(route, &QPushButton::clicked, this, [this] {
		RouteObsThroughLiveDelay(window());
		UpdateBanner();
	});

	ShowOffline();
	UpdateBanner();
	QPointer<Dock> self(this);
	events_ = std::make_unique<EventStream>(
		[self](const std::string &json) {
			QMetaObject::invokeMethod(
				self, [self, json] { if (self) self->ShowState(json); }, Qt::QueuedConnection);
		},
		[self] {
			QMetaObject::invokeMethod(
				self, [self] { if (self) self->ShowOffline(); }, Qt::QueuedConnection);
		});
}

Dock::~Dock() = default;

void Dock::Act(const std::string &action)
{
	QPointer<Dock> self(this);
	std::thread([self, action] {
		int status = 0;
		std::string body = Request("POST", "/" + action, &status);
		// Error codes from the proxy are translated on the UI thread.
		std::string code = status == 0 ? "no_response" : status != 200 ? JsonString(body, "error") : "";
		QMetaObject::invokeMethod(
			self,
			[self, code] {
				if (self)
					self->ShowError(code.empty() ? QString() : T(("Err." + code).c_str()));
			},
			Qt::QueuedConnection);
	}).detach();
}

void Dock::Hotkey(const std::string &action)
{
	Act(action == "arm" ? "arm?delay=" + std::to_string(delay_->value()) : action);
}

void Dock::ShowError(const QString &msg)
{
	error_->setText(msg);
	error_->setVisible(!msg.isEmpty());
}

void Dock::UpdateBanner()
{
	banner_->setVisible(!ObsRoutedToLiveDelay());
}

static void Pill(QLabel *l, const QString &text, const char *bg)
{
	l->setText(text);
	l->setStyleSheet(QString("font-size: 15px; font-weight: 600; border-radius: 6px; color: white; background: %1;").arg(bg));
}

void Dock::ShowOffline()
{
	last_.clear();
	Pill(status_, T("Dock.Offline"), "#5a5f6b");
	detail_->setText(LastError().empty() ? T("Dock.Starting") : QString::fromStdString(LastError()));
	fill_->hide();
	dests_->hide();
	for (QPushButton *b : {arm_, activate_, cut_, cut_after_, disarm_})
		b->setEnabled(false);
	start_->show();
}

void Dock::ShowDestinations(const std::string &j)
{
	// Rebuild the rows; there are only a handful and state changes are rare.
	while (QLayoutItem *item = destGrid_->takeAt(0)) {
		delete item->widget();
		delete item;
	}
	// One bitrate point a second per line.
	const auto now = std::chrono::steady_clock::now();
	const bool sample = now - lastSample_ >= std::chrono::milliseconds(900);
	if (sample)
		lastSample_ = now;
	auto history = [&](const std::string &key, double v) -> std::deque<double> & {
		auto &h = history_[key];
		if (sample) {
			h.push_back(v);
			if (h.size() > kHistory)
				h.pop_front();
		}
		return h;
	};

	int row = 0;
	{
		const bool input = JsonBool(j, "input");
		const double kbps = input ? JsonNumber(j, "in_kbps") : 0;
		auto *dot = new QLabel(QString::fromUtf8("●"));
		dot->setStyleSheet(QString("color: %1;").arg(input ? "#5ac8fa" : "#7a7f8c"));
		auto *name = new QLabel(T("Dock.InputRow"));
		auto *rate = new QLabel(input ? QString("%1 kb/s").arg(kbps, 0, 'f', 0) : T("Dock.NoSignalShort"));
		rate->setStyleSheet("color: palette(placeholder-text);");
		destGrid_->addWidget(dot, row, 0);
		destGrid_->addWidget(name, row, 1);
		destGrid_->addWidget(new Sparkline(history("in", kbps), QColor("#5ac8fa")), row, 2);
		destGrid_->addWidget(rate, row, 3);
		row++;
	}
	size_t p = 0;
	int index = 0;
	while ((p = j.find("\"name\":\"", p)) != std::string::npos) {
		const std::string state = JsonString(j, "state", p);
		const bool on = JsonBool(j, "on", p);
		const double kbps = JsonNumber(j, "kbps", 0, p);
		const std::string error = JsonString(j, "error", p);
		const char *color = state == "live" ? "#3ddc84" : state == "error" ? "#ff5a4f" : state == "stopped" ? "#7a7f8c" : "#ffbf3c";

		auto *dot = new QLabel(QString::fromUtf8("●"));
		dot->setStyleSheet(QString("color: %1;").arg(color));
		const std::string destName = JsonString(j, "name", p);
		auto *name = new QLabel(QString::fromStdString(destName));
		QString st = T(("Dest." + state).c_str());
		if (kbps > 0)
			st += QString(" · %1 kb/s").arg(kbps, 0, 'f', 0);
		auto *stateLabel = new QLabel(st);
		stateLabel->setStyleSheet("color: palette(placeholder-text);");
		if (!error.empty())
			stateLabel->setToolTip(QString::fromStdString(error));
		auto *toggle = new QPushButton(on ? T("Dock.DestStop") : T("Dock.DestStart"));
		toggle->setToolTip(on ? T("Dock.DestStopTip") : T("Dock.DestStartTip"));
		toggle->setIcon(MakeIcon(on ? Icon::Stop : Icon::Play, QColor(on ? "#ff5a4f" : "#3ddc84")));
		toggle->setIconSize(QSize(12, 12));
		toggle->setCursor(Qt::PointingHandCursor);
		const std::string path = std::string(on ? "dest/stop" : "dest/start") + "?i=" + std::to_string(index);
		connect(toggle, &QPushButton::clicked, this, [this, path] { Act(path); });

		destGrid_->addWidget(dot, row, 0);
		destGrid_->addWidget(name, row, 1);
		destGrid_->addWidget(new Sparkline(history(destName, state == "live" ? kbps : 0), QColor(color)), row, 2);
		destGrid_->addWidget(stateLabel, row, 3);
		destGrid_->addWidget(toggle, row, 4);
		p += 8;
		row++;
		index++;
	}
	if (index == 0) {
		auto *none = new QLabel(T("Dock.NoPlatforms"));
		none->setWordWrap(true);
		destGrid_->addWidget(none, row, 0, 1, 5);
	}
	dests_->show();
}

void Dock::ShowState(const std::string &j)
{
	last_ = j;
	start_->hide();
	UpdateBanner();
	const bool delayed = JsonString(j, "mode") == "delayed";
	const bool input = JsonBool(j, "input");
	const bool armed = JsonBool(j, "armed");
	const bool cut_pending = JsonBool(j, "cut_pending");
	const double arm_delay = JsonNumber(j, "arm_delay", -1);
	const double delay = JsonNumber(j, "delay");
	const double buffer = JsonNumber(j, "buffer_s");

	if (!delay_set_) {
		delay_set_ = true;
		delay_->setValue(int(arm_delay > 0 ? arm_delay : JsonNumber(j, "default_delay", 15)));
	}

	if (delayed)
		Pill(status_, T("Dock.StateDelayed").arg(delay, 0, 'f', 0), "#c62828");
	else if (!input)
		Pill(status_, T("Dock.StateWaiting"), "#3b4a63");
	else if (armed)
		Pill(status_, T("Dock.StateArmed").arg(arm_delay, 0, 'f', 0), "#b26a00");
	else if (arm_delay > 0)
		Pill(status_, T("Dock.StateArming").arg(buffer, 0, 'f', 0).arg(arm_delay, 0, 'f', 0), "#b26a00");
	else
		Pill(status_, T("Dock.StateLive"), "#2e7d32");

	const bool filling = input && !delayed && arm_delay > 0 && !armed;
	fill_->setVisible(filling);
	if (filling) {
		fill_->setRange(0, int(arm_delay * 10));
		fill_->setValue(int(buffer * 10));
	}

	QString d;
	if (!input)
		d = T(delayed ? "Dock.DrainingTail" : "Dock.WaitingHint");
	if (cut_pending)
		d += T("Dock.CutPending");
	detail_->setText(d);
	detail_->setVisible(!d.isEmpty());
	ShowDestinations(j);

	arm_->setEnabled(!delayed);
	activate_->setEnabled(!delayed && armed);
	cut_->setEnabled(delayed);
	cut_after_->setEnabled(delayed && !cut_pending);
	disarm_->setEnabled(!delayed && arm_delay > 0);
}

} // namespace livedelay
