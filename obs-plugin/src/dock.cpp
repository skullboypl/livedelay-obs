#include "dock.hpp"
#include "contrast.hpp"
#include "theme.hpp"

#include <cstdlib>
#include "client.hpp"
#include "encode.hpp"
#include "i18n.hpp"
#include "icons.hpp"
#include "limits.hpp"
#include "outputs.hpp"
#include "settings.hpp"
#include "update.hpp"

#include <obs-frontend-api.h>

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
#include <QTimer>
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

// What an action means, not what colour it is: the palette decides that.
enum class Accent { None, Tally, Live, Arm };

static QColor AccentColor(const Palette &p, Accent a)
{
	switch (a) {
	case Accent::Tally:
		return p.tally;
	case Accent::Live:
		return p.live;
	case Accent::Arm:
		return p.arm;
	case Accent::None:
	default:
		return p.text;
	}
}

// One button, dressed for the current theme. The action the state is waiting
// for is filled; the rest stay outlined, and plain tools stay quiet.
static void StyleButton(QPushButton *b, const Palette &p)
{
	const auto accent = Accent(b->property("ldAccent").toInt());
	const bool primary = b->property("ldPrimary").toBool();
	QColor fg = p.text;
	if (accent == Accent::None) {
		// Under "same as OBS" an unaccented button is simply an OBS button.
		b->setStyleSheet(p.painted ? QuietButton(p) : QString());
	} else {
		const QColor c = AccentColor(p, accent);
		b->setStyleSheet(primary ? FilledButton(c) : GhostButton(c, p.text));
		fg = primary ? InkOn(c) : c;
	}
	b->setIcon(MakeIcon(Icon(b->property("ldIcon").toInt()), fg));
}

// Buttons remember their text keys so Retranslate() can switch language live,
// and their accent so the theme can be swapped without rebuilding the dock.
static QPushButton *Button(const char *text, const char *tip, Icon icon, Accent accent = Accent::None)
{
	auto *b = new QPushButton(T(text));
	b->setToolTip(T(tip));
	b->setProperty("ldText", text);
	b->setProperty("ldTip", tip);
	b->setProperty("ldAccent", int(accent));
	b->setProperty("ldIcon", int(icon));
	b->setIconSize(QSize(16, 16));
	b->setMinimumHeight(32);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}

// What a limits warning is about, in numbers rather than words, so dismissing it
// survives a restart and a language change but not a settings change.
static QString LimitsKey(const QStringList &problems)
{
	const ObsOutput o = ReadObsOutput();
	return QStringLiteral("%1/%2/%3/%4/%5")
		.arg(o.video_kbps)
		.arg(o.audio_kbps)
		.arg(o.keyint_sec)
		.arg(o.rate_control.isEmpty() ? QStringLiteral("-") : o.rate_control)
		.arg(problems.size());
}

// Everything the running engine fixed at startup: its addresses and its
// platform list. Changing any of it needs the proxy itself to come back up,
// unlike a delay or a theme, which are picked up as they are.
static std::string EngineText(const Settings &s)
{
	const std::string all = s.Text();
	const size_t at = all.find("[dest ");
	return s.listen + "|" + s.api + "|" + (at == std::string::npos ? std::string() : all.substr(at));
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
	setObjectName("ldDock");
	setAttribute(Qt::WA_StyledBackground, true);
	{
		const Settings s = Settings::Load(ConfigPath());
		theme_ = ParseDockTheme(s.dock_theme);
		pal_ = ThemePalette(theme_);
		language_ = s.language;
		limitsAck_ = QString::fromStdString(s.limits_ack);
		engine_text_ = EngineText(s);
		configStamp_ = ConfigStamp();
	}

	// Shown while OBS streams somewhere else than LiveDelay.
	banner_ = new QFrame;
	banner_->setObjectName("ldBanner");
	auto *bannerText = Label("Dock.NotRouted");
	bannerText->setWordWrap(true);
	auto *route = Button("Dock.Route", "Dock.Route", Icon::Play, Accent::Arm);
	auto *bannerLayout = new QHBoxLayout(banner_);
	bannerLayout->addWidget(bannerText, 1);
	bannerLayout->addWidget(route);
	banner_->hide();

	// Shown while OBS's stream settings exceed what a platform accepts.
	limits_ = new QFrame;
	limits_->setObjectName("ldLimits");
	limitsText_ = new QLabel;
	limitsText_->setWordWrap(true);
	limitsFit_ = Button("Limits.Fit", "Limits.FitTip", Icon::Settings, Accent::Tally);
	limitsHide_ = Button("Limits.Hide", "Limits.HideTip", Icon::Disarm);
	auto *limitsLayout = new QHBoxLayout(limits_);
	limitsLayout->addWidget(limitsText_, 1);
	limitsLayout->addWidget(limitsFit_);
	limitsLayout->addWidget(limitsHide_);
	limits_->hide();

	status_ = new QLabel;
	status_->setAlignment(Qt::AlignCenter);
	status_->setMinimumHeight(36);
	detail_ = new QLabel;
	detail_->setWordWrap(true);
	detail_->setAlignment(Qt::AlignCenter);
	error_ = new QLabel;
	error_->setWordWrap(true);
	error_->hide();

	delay_ = new QSpinBox;
	delay_->setRange(1, 3600);
	delay_->setValue(15);
	delay_->setSuffix(" s");
	delay_->setToolTip(T("Dock.DelayTip"));
	delay_->setMinimumHeight(32);
	arm_ = Button("Dock.Arm", "Dock.ArmTip", Icon::Arm, Accent::Arm);
	activate_ = Button("Dock.Activate", "Dock.ActivateTip", Icon::Activate, Accent::Tally);
	cut_ = Button("Dock.Cut", "Dock.CutTip", Icon::Live, Accent::Live);
	cut_after_ = Button("Dock.CutAfter", "Dock.CutAfterTip", Icon::LiveAfter, Accent::Live);
	disarm_ = Button("Dock.Disarm", "Dock.DisarmTip", Icon::Disarm);
	start_ = Button("Dock.Restart", "Dock.RestartTip", Icon::Restart, Accent::Arm);
	auto *settings = Button("Dock.Settings", "Dock.SettingsTip", Icon::Settings);
	auto *panel = Button("Dock.Panel", "Dock.PanelTip", Icon::Panel);
	auto *preview = Button("Dock.Preview", "Dock.PreviewTip", Icon::Preview);
	auto *help = Button("Dock.Help", "Dock.HelpTip", Icon::Help);
	fill_ = new QProgressBar;
	fill_->setTextVisible(false);
	fill_->setMaximumHeight(6);
	fill_->hide();

	// Platforms with their own Start / Stop.
	dests_ = new QFrame;
	dests_->setObjectName("ldDests");
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
	// Four tools in one row would squeeze their labels out of a narrow dock.
	auto *toolsRow = new QGridLayout;
	toolsRow->addWidget(settings, 0, 0);
	toolsRow->addWidget(panel, 0, 1);
	toolsRow->addWidget(preview, 1, 0);
	toolsRow->addWidget(help, 1, 1);

	auto *layout = new QVBoxLayout(this);
	AttachUpdateBanner(this, layout);
	layout->addWidget(banner_);
	layout->addWidget(limits_);
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
	connect(settings, &QPushButton::clicked, this, [this] {
		ShowSettings(window());
		UpdateBanner();
	});
	connect(limitsFit_, &QPushButton::clicked, this, [this] {
		const bool ok = FitEncodeForObsMode();
		UpdateBanner();
		ShowError(ok ? QString() : T("Settings.SaveFailed"));
	});
	connect(limitsHide_, &QPushButton::clicked, this, [this] {
		// Remembered in the ini, so the same warning does not come back with
		// the next OBS. Change the settings it is about and it returns.
		limitsAck_ = limitsKey_;
		SaveLimitsAck(limitsAck_.toStdString());
		configStamp_ = ConfigStamp();
		UpdateBanner();
	});
	// OBS settings can change behind our back (Settings dialog, profile switch).
	auto *timer = new QTimer(this);
	connect(timer, &QTimer::timeout, this, [this] { UpdateBanner(); });
	timer->start(10000);
	// So can livedelay.ini: the web panel and the standalone app write the same
	// file. Watching its write time is cheaper than re-reading it every second.
	auto *iniTimer = new QTimer(this);
	connect(iniTimer, &QTimer::timeout, this, [this] {
		const unsigned long long stamp = ConfigStamp();
		if (stamp == 0 || stamp == configStamp_)
			return;
		configStamp_ = stamp;
		ReloadConfig();
	});
	iniTimer->start(1000);
	connect(preview, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QString::fromStdString(LivePreviewUrl()))); });
	// The same settings page the standalone app opens; the engine serves it
	// whether the stream runs through this plugin or through the app.
	connect(panel, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(QString::fromStdString(PanelUrl()))); });
	connect(help, &QPushButton::clicked, this, [this] { ShowHelp(window()); });
	connect(route, &QPushButton::clicked, this, [this] {
		RouteObsThroughLiveDelay(window());
		UpdateBanner();
	});

	ApplyTheme();
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

// Repaints everything the plugin owns. What it does not own — OBS's own
// widgets around the dock — is left alone on purpose.
void Dock::ApplyTheme()
{
	setStyleSheet(DockSheet(pal_));
	// Only the buttons Button() made: the platform rows carry their own style
	// and are rebuilt from scratch on the next state anyway.
	for (QPushButton *b : findChildren<QPushButton *>()) {
		if (b->property("ldIcon").isValid())
			StyleButton(b, pal_);
	}
	error_->setStyleSheet(QStringLiteral("color: %1;").arg(pal_.tally.name()));
	dests_->setStyleSheet(CardSheet("ldDests", pal_, pal_.line, false));
	banner_->setStyleSheet(CardSheet("ldBanner", pal_, pal_.arm));
	if (last_.empty())
		ShowOffline();
	else
		ShowState(last_);
	UpdateBanner();
}

// The action this state is waiting for; everything else goes quiet.
void Dock::SetPrimary(QPushButton *primary)
{
	for (QPushButton *b : {arm_, activate_, cut_, cut_after_, disarm_, start_}) {
		const bool was = b->property("ldPrimary").toBool();
		const bool now = b == primary;
		if (was == now)
			continue;
		b->setProperty("ldPrimary", now);
		StyleButton(b, pal_);
	}
}

// Everything the ini on disk can change under the dock: the web panel, the
// standalone app and this plugin's own settings window all write that one file.
void Dock::ReloadConfig()
{
	const Settings s = Settings::Load(ConfigPath());
	configStamp_ = ConfigStamp();
	const DockTheme theme = ParseDockTheme(s.dock_theme);
	if (theme != theme_) {
		theme_ = theme;
		pal_ = ThemePalette(theme);
		ApplyTheme();
	}
	limitsAck_ = QString::fromStdString(s.limits_ack);
	if (s.language != language_) {
		language_ = s.language;
		ApplyLanguage(s.language); // retranslates the dock through plugin.cpp
	}
	// Platforms edited elsewhere leave the engine on its old list and OBS's
	// per-platform encoders stale. Both come back mid-stream only at the cost
	// of cutting the stream, so wait for idle.
	const std::string engine = EngineText(s);
	if (engine != engine_text_) {
		engine_text_ = engine;
		if (!obs_frontend_streaming_active()) {
			ShowError(QString::fromStdString(StartProxy()));
			OutputsReloadConfig();
		}
	}
	// A default delay changed elsewhere is only safe to adopt while nothing is
	// armed — otherwise it would overwrite what the user is about to arm.
	if (!last_.empty() && JsonNumber(last_, "arm_delay", -1) <= 0 && s.delay >= delay_->minimum() &&
	    s.delay <= delay_->maximum())
		delay_->setValue(s.delay);
	UpdateBanner();
}

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
	const std::string prefix = "arm?delay=";
	if (action.rfind(prefix, 0) == 0) {
		const int sec = std::atoi(action.c_str() + prefix.size());
		if (sec >= delay_->minimum() && sec <= delay_->maximum())
			delay_->setValue(sec);
		Act(action);
		return;
	}
	Act(action == "arm" ? prefix + std::to_string(delay_->value()) : action);
}

void Dock::ShowError(const QString &msg)
{
	error_->setText(msg);
	error_->setVisible(!msg.isEmpty());
}

void Dock::UpdateBanner()
{
	const bool routed = ObsRoutedToLiveDelay();
	banner_->setVisible(!routed);
	QStringList problems = routed ? EncodeProblems() : QStringList();
	const QString info = MultiEncodeInfo();
	if (problems.isEmpty() && !info.isEmpty()) {
		// Not a problem, just something worth knowing: a quiet card, no accent.
		limitsText_->setText(info);
		limits_->setStyleSheet(CardSheet("ldLimits", pal_, pal_.muted));
		limitsFit_->setVisible(false);
		limitsHide_->setVisible(false);
		limits_->setVisible(true);
		return;
	}
	limits_->setStyleSheet(CardSheet("ldLimits", pal_, pal_.tally));
	limitsKey_ = problems.isEmpty() ? QString() : LimitsKey(problems);
	const bool show = !problems.isEmpty() && limitsKey_ != limitsAck_;
	limitsFit_->setVisible(show);
	limitsHide_->setVisible(show);
	limitsText_->setText(problems.isEmpty() ? QString() : ("<b>" + T("Limits.Title") + "</b><br>" + problems.join("<br>")));
	limits_->setVisible(show);
}

// The state pill: filled in the colour of what is on air, quiet when nothing is.
static void Pill(QLabel *l, const QString &text, const QColor &bg, const Palette &p, bool filled = true)
{
	l->setText(text);
	l->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700; letter-spacing: .04em; border-radius: 6px;"
					" padding: 6px 10px; color: %1; background: %2;")
				 .arg(filled ? InkOn(bg).name() : p.text.name(), filled ? bg.name() : Rgba(p.muted, .20)));
}

void Dock::ShowOffline()
{
	last_.clear();
	Pill(status_, T("Dock.Offline"), pal_.muted, pal_, false);
	detail_->setText(LastError().empty() ? T("Dock.Starting") : QString::fromStdString(LastError()));
	fill_->hide();
	dests_->hide();
	for (QPushButton *b : {arm_, activate_, cut_, cut_after_, disarm_})
		b->setEnabled(false);
	start_->show();
	SetPrimary(start_);
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
		const QColor inColor = input ? pal_.text : pal_.muted;
		auto *dot = new QLabel(QString::fromUtf8("●"));
		// What OBS feeds in is none of the three on-air states, so it stays in
		// the panel's own greys: bright when there is signal, dim when not.
		dot->setStyleSheet(QStringLiteral("color: %1;").arg(inColor.name()));
		auto *name = new QLabel(T("Dock.InputRow"));
		auto *rate = new QLabel(input ? QString("%1 kb/s").arg(kbps, 0, 'f', 0) : T("Dock.NoSignalShort"));
		rate->setStyleSheet(QStringLiteral("color: %1;").arg(pal_.muted.name()));
		destGrid_->addWidget(dot, row, 0);
		destGrid_->addWidget(name, row, 1);
		destGrid_->addWidget(new Sparkline(history("in", kbps), inColor), row, 2);
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
		const QColor color = state == "live"	 ? pal_.live
				     : state == "error"	 ? pal_.tally
				     : state == "stopped" ? pal_.muted
							  : pal_.arm;

		auto *dot = new QLabel(QString::fromUtf8("●"));
		dot->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
		const std::string destName = JsonString(j, "name", p);
		auto *name = new QLabel(QString::fromStdString(destName));
		QString st = T(("Dest." + state).c_str());
		if (kbps > 0)
			st += QString(" · %1 kb/s").arg(kbps, 0, 'f', 0);
		auto *stateLabel = new QLabel(st);
		stateLabel->setStyleSheet(QStringLiteral("color: %1;").arg(pal_.muted.name()));
		if (!error.empty())
			stateLabel->setToolTip(QString::fromStdString(error));
		// Per-platform Start / Stop never shouts: the dock's loud button is
		// the delay action, not one of a handful of rows.
		auto *toggle = new QPushButton(on ? T("Dock.DestStop") : T("Dock.DestStart"));
		toggle->setToolTip(on ? T("Dock.DestStopTip") : T("Dock.DestStartTip"));
		toggle->setStyleSheet(GhostButton(on ? pal_.tally : pal_.live, pal_.text));
		toggle->setIcon(MakeIcon(on ? Icon::Stop : Icon::Play, on ? pal_.tally : pal_.live));
		toggle->setIconSize(QSize(12, 12));
		toggle->setCursor(Qt::PointingHandCursor);
		const std::string path = std::string(on ? "dest/stop" : "dest/start") + "?i=" + std::to_string(index);
		const int destIndex = index;
		const bool turningOn = !on;
		connect(toggle, &QPushButton::clicked, this, [this, path, destIndex, turningOn] {
			if (turningOn)
				OutputStartDest(size_t(destIndex));
			else
				OutputStopDest(size_t(destIndex));
			Act(path);
		});

		destGrid_->addWidget(dot, row, 0);
		destGrid_->addWidget(name, row, 1);
		destGrid_->addWidget(new Sparkline(history(destName, state == "live" ? kbps : 0), color), row, 2);
		destGrid_->addWidget(stateLabel, row, 3);
		destGrid_->addWidget(toggle, row, 4);
		p += 8;
		row++;
		index++;
	}
	if (index == 0) {
		auto *none = new QLabel(T("Dock.NoPlatforms"));
		none->setWordWrap(true);
		none->setStyleSheet(QStringLiteral("color: %1;").arg(pal_.muted.name()));
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

	// Show configured Arm seconds, not measured keyframe lag (always a bit higher).
	const double shown_delay = arm_delay > 0 ? arm_delay : delay;
	if (delayed)
		Pill(status_, T("Dock.StateDelayed").arg(shown_delay, 0, 'f', 0), pal_.tally, pal_);
	else if (!input)
		Pill(status_, T("Dock.StateWaiting"), pal_.muted, pal_, false);
	else if (armed)
		Pill(status_, T("Dock.StateArmed").arg(arm_delay, 0, 'f', 0), pal_.arm, pal_);
	else if (arm_delay > 0)
		Pill(status_, T("Dock.StateArming").arg(buffer, 0, 'f', 0).arg(arm_delay, 0, 'f', 0), pal_.arm, pal_);
	else
		Pill(status_, T("Dock.StateLive"), pal_.live, pal_);

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
	// Whatever this state is heading towards is the one loud button.
	SetPrimary(delayed ? cut_ : arm_delay > 0 ? activate_ : arm_);
}

} // namespace livedelay
