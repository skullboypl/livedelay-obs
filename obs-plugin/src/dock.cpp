#include "dock.hpp"
#include "client.hpp"
#include "i18n.hpp"
#include "settings.hpp"

#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <thread>

namespace livedelay {

static QPushButton *Button(const char *text, const char *tip)
{
	auto *b = new QPushButton(T(text));
	b->setToolTip(T(tip));
	b->setMinimumHeight(30);
	return b;
}

Dock::Dock(QWidget *parent) : QWidget(parent)
{
	// Shown while OBS streams somewhere else than LiveDelay.
	banner_ = new QFrame;
	banner_->setObjectName("ldBanner");
	banner_->setStyleSheet("#ldBanner { background: rgba(255, 191, 60, .14); border: 1px solid rgba(255, 191, 60, .45); border-radius: 6px; }");
	auto *bannerText = new QLabel(T("Dock.NotRouted"));
	bannerText->setWordWrap(true);
	auto *route = new QPushButton(T("Dock.Route"));
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
	arm_ = Button("Dock.Arm", "Dock.ArmTip");
	activate_ = Button("Dock.Activate", "Dock.ActivateTip");
	cut_ = Button("Dock.Cut", "Dock.CutTip");
	cut_after_ = Button("Dock.CutAfter", "Dock.CutAfterTip");
	disarm_ = Button("Dock.Disarm", "Dock.DisarmTip");
	start_ = Button("Dock.Restart", "Dock.RestartTip");
	auto *settings = Button("Dock.Settings", "Dock.SettingsTip");
	auto *preview = Button("Dock.Preview", "Dock.PreviewTip");
	auto *help = Button("Dock.Help", "Dock.HelpTip");
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
	size_t p = 0;
	int row = 0;
	while ((p = j.find("\"name\":\"", p)) != std::string::npos) {
		const std::string state = JsonString(j, "state", p);
		const bool on = JsonBool(j, "on", p);
		const double kbps = JsonNumber(j, "kbps", 0, p);
		const std::string error = JsonString(j, "error", p);
		const char *color = state == "live" ? "#3ddc84" : state == "error" ? "#ff5a4f" : state == "stopped" ? "#7a7f8c" : "#ffbf3c";

		auto *dot = new QLabel(QString::fromUtf8("●"));
		dot->setStyleSheet(QString("color: %1;").arg(color));
		auto *name = new QLabel(QString::fromStdString(JsonString(j, "name", p)));
		QString st = T(("Dest." + state).c_str());
		if (kbps > 0)
			st += QString(" · %1 kb/s").arg(kbps, 0, 'f', 0);
		auto *stateLabel = new QLabel(st);
		stateLabel->setStyleSheet("color: palette(placeholder-text);");
		if (!error.empty())
			stateLabel->setToolTip(QString::fromStdString(error));
		auto *toggle = new QPushButton(on ? T("Dock.DestStop") : T("Dock.DestStart"));
		toggle->setToolTip(on ? T("Dock.DestStopTip") : T("Dock.DestStartTip"));
		const std::string path = std::string(on ? "dest/stop" : "dest/start") + "?i=" + std::to_string(row);
		connect(toggle, &QPushButton::clicked, this, [this, path] { Act(path); });

		destGrid_->addWidget(dot, row, 0);
		destGrid_->addWidget(name, row, 1);
		destGrid_->addWidget(stateLabel, row, 2);
		destGrid_->addWidget(toggle, row, 3);
		p += 8;
		row++;
	}
	if (row == 0) {
		auto *none = new QLabel(T("Dock.NoPlatforms"));
		none->setWordWrap(true);
		destGrid_->addWidget(none, 0, 0, 1, 4);
	}
	dests_->show();
}

void Dock::ShowState(const std::string &j)
{
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
