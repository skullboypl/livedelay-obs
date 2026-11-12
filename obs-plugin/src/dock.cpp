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

#include <QApplication>
#include <QDesktopServices>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <chrono>
#include <thread>

namespace livedelay {

// Set by the dock when it is short of height: the pill and everything else
// take less room. One dock per OBS, so one flag.
static bool s_compact = false;

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
	// Every section counts: [input …] sits before the platforms and decides
	// which slot feeds which, so an input changed in the web panel or the app
	// needs the same restart as a platform does.
	const std::string all = s.Text();
	const size_t at = all.find("\n[");
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
	Reflow();
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
		obsLive_ = s.obs_live;
		ReadInputs(s);
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
	status_->setWordWrap(true);
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
	// Other seconds than the armed ones turn Arm into "arm again".
	connect(delay_, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
		if (!last_.empty())
			ShowState(std::string(last_));
	});
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
	// Two by two while there is room, one under another in a narrow dock:
	// Reflow() places them.
	bigGrid_ = new QGridLayout;
	toolsGrid_ = new QGridLayout;
	tools_ = {settings, panel, preview, help};
	outputsFolded_ = QSettings(QStringLiteral("skullboypl"), QStringLiteral("livedelay-obs"))
				 .value(QStringLiteral("dock/outputsFolded"), false)
				 .toBool();

	// Everything sits in a scroll area: a dock dragged short gets a scroll bar
	// instead of rows squeezed flat and text cut off.
	auto *body = new QWidget;
	auto *layout = new QVBoxLayout(body);
	AttachUpdateBanner(body, layout);
	layout->addWidget(banner_);
	layout->addWidget(limits_);
	layout->addWidget(status_);
	layout->addWidget(fill_);
	layout->addWidget(detail_);
	layout->addLayout(armRow);
	layout->addLayout(bigGrid_);
	layout->addWidget(dests_);
	layout->addWidget(start_);
	layout->addLayout(toolsGrid_);
	layout->addWidget(error_);
	layout->addStretch();
	auto *scroll = new QScrollArea;
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	scroll->setWidget(body);
	scroll->viewport()->setAutoFillBackground(false);
	body->setAutoFillBackground(false);
	scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; }"));
	body_ = body;
	scroll_ = scroll;
	normalMargins_ = layout->contentsMargins();
	normalSpacing_ = layout->spacing();
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->addWidget(scroll);
	setMinimumWidth(220);
	Reflow();

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
		if (enginePending_ && !obs_frontend_streaming_active()) {
			ReloadConfig();
			return;
		}
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
	ReadInputs(s);
	if (s.language != language_) {
		language_ = s.language;
		ApplyLanguage(s.language); // retranslates the dock through plugin.cpp
	}
	// Platforms edited elsewhere leave the engine on its old list and OBS's
	// per-platform encoders stale. Both come back mid-stream only at the cost
	// of cutting the stream, so wait for idle.
	if (s.obs_live != obsLive_) {
		obsLive_ = s.obs_live;
		// The second encoder holds the slot the OBS publish needs. Drop it
		// without restarting the proxy, so the open stream can feed the preview.
		if (!s.single_pc)
			OutputsReloadConfig();
	}
	const std::string engine = EngineText(s);
	if (engine != engine_text_) {
		if (!obs_frontend_streaming_active()) {
			engine_text_ = engine;
			enginePending_ = false;
			ShowError(QString::fromStdString(StartProxy()));
			OutputsReloadConfig();
		} else {
			// Remembered, not forgotten: the timer applies it once the stream
			// ends. Taking the new text as current here used to lose the change
			// for good.
			enginePending_ = true;
		}
	} else {
		enginePending_ = false;
	}
	// A default delay changed elsewhere is only safe to adopt while nothing is
	// armed — otherwise it would overwrite what the user is about to arm.
	if (!last_.empty() && JsonNumber(last_, "arm_delay", -1) <= 0 && s.delay >= delay_->minimum() &&
	    s.delay <= delay_->maximum())
		delay_->setValue(s.delay);
	UpdateBanner();
}

// The big buttons stay two by two: a narrow dock gets shorter words, not a
// taller column. The tools become one row of icons when words do not fit or
// height is short; their names stay in the tooltips.
void Dock::Reflow()
{
	QPushButton *big[] = {activate_, cut_, cut_after_, disarm_};
	for (int i = 0; i < 4; ++i) {
		bigGrid_->removeWidget(big[i]);
		bigGrid_->addWidget(big[i], i / 2, i % 2);
	}
	cut_after_->setText(T(narrow_ ? "Inputs.CutAfterShort" : "Dock.CutAfter"));
	const bool icons = narrow_ || compact_;
	const int cols = icons ? int(tools_.size()) : 2;
	for (int i = 0; i < int(tools_.size()); ++i) {
		QPushButton *b = tools_[size_t(i)];
		toolsGrid_->removeWidget(b);
		toolsGrid_->addWidget(b, i / cols, i % cols);
		b->setText(icons ? QString() : T(b->property("ldText").toByteArray().constData()));
	}
}

// Short of height, the dock tightens before it scrolls: smaller buttons and
// gaps, the hint line folded into a tooltip.
void Dock::ApplyDensity()
{
	s_compact = compact_;
	QLayout *lay = body_->layout();
	lay->setSpacing(compact_ ? 3 : normalSpacing_);
	lay->setContentsMargins(compact_ ? QMargins(5, 4, 5, 4) : normalMargins_);
	const int h = compact_ ? 24 : 32;
	for (QPushButton *b : {arm_, activate_, cut_, cut_after_, disarm_, start_})
		b->setMinimumHeight(h);
	for (QPushButton *b : tools_)
		b->setMinimumHeight(h);
	delay_->setMinimumHeight(h);
	status_->setMinimumHeight(compact_ ? 22 : 36);
	destGrid_->setContentsMargins(compact_ ? QMargins(6, 2, 6, 2) : QMargins(8, 6, 8, 6));
	destGrid_->setVerticalSpacing(compact_ ? 1 : -1);
	bigGrid_->setSpacing(compact_ ? 3 : -1);
	toolsGrid_->setSpacing(compact_ ? 3 : -1);
}

void Dock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	FitWidth();
}

// The wide form is used while it fits. What "fits" means depends on what is
// on screen — a delay per input needs more room than a list of platforms — so
// it is measured, not assumed: the width the wide form asked for when it last
// overflowed is the width at which it is tried again.
void Dock::FitWidth()
{
	if (fitting_ || !body_ || !scroll_)
		return;
	fitting_ = true;
	const int have = scroll_->viewport()->width();
	auto redraw = [this] {
		ApplyDensity();
		Reflow();
		if (last_.empty())
			ShowOffline();
		else
			ShowState(std::string(last_));
		body_->layout()->activate();
	};
	if (narrow_ && wideNeeds_ > 0 && have >= wideNeeds_) {
		narrow_ = false;
		redraw();
	}
	if (!narrow_) {
		const int need = body_->minimumSizeHint().width();
		if (need > have) {
			wideNeeds_ = need;
			narrow_ = true;
			redraw();
		}
	}
	// Height the same way: the roomy form while it fits, the tight one when
	// it does not, and scrolling only for what is left after that.
	const int tall = scroll_->viewport()->height();
	if (compact_ && roomyNeeds_ > 0 && tall >= roomyNeeds_) {
		compact_ = false;
		redraw();
	}
	if (!compact_) {
		const int need = body_->minimumSizeHint().height();
		if (need > tall) {
			roomyNeeds_ = need;
			compact_ = true;
			redraw();
		}
	}
	fitting_ = false;
}

// Which inputs are a platform's own encode and which are shared: the dock
// draws the first kind as the platform's row and the second as a heading.
void Dock::ReadInputs(const Settings &s)
{
	perInput_ = s.PerInputDelay();
	ownInputs_.clear();
	sharedInputs_.clear();
	for (const Input &in : s.EffectiveInputs())
		(in.own ? ownInputs_ : sharedInputs_).insert(in.name);
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
	QString info = MultiEncodeInfo();
	const QString cap = TwitchCapInfo();
	if (!cap.isEmpty())
		info = info.isEmpty() ? cap : info + QStringLiteral("<br>") + cap;
	limits_->setToolTip(cap.isEmpty() ? QString() : T("Limits.TwitchCapTip"));
	if (problems.isEmpty() && !info.isEmpty()) {
		// Not a problem, just something worth knowing: a quiet card, no accent.
		// It closes like a warning does, and comes back only with other news.
		limitsKey_ = QStringLiteral("info%1").arg(qHash(info));
		const bool show = limitsKey_ != limitsAck_;
		limitsText_->setText(info);
		limits_->setStyleSheet(CardSheet("ldLimits", pal_, pal_.muted));
		limitsFit_->setVisible(false);
		limitsHide_->setVisible(show);
		limits_->setVisible(show);
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
	l->setStyleSheet(QStringLiteral("font-size: %3px; font-weight: 700; letter-spacing: .04em; border-radius: 6px;"
					" padding: %4; color: %1; background: %2;")
				 .arg(filled ? InkOn(bg).name() : p.text.name(), filled ? bg.name() : Rgba(p.muted, .20))
				 .arg(s_compact ? 12 : 15)
				 .arg(s_compact ? QStringLiteral("2px 8px") : QStringLiteral("6px 10px")));
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
	// A row rebuilt between a button going down and coming up swallows the
	// click — and while a stream runs, the state changes every second.
	if (QApplication::mouseButtons() != Qt::NoButton && dests_->isVisible() &&
	    dests_->rect().contains(dests_->mapFromGlobal(QCursor::pos()))) {
		if (!rowsDeferred_) {
			rowsDeferred_ = true;
			QTimer::singleShot(150, this, [this] {
				rowsDeferred_ = false;
				if (!last_.empty())
					ShowDestinations(std::string(last_));
			});
		}
		return;
	}
	QString focusKey;
	if (QWidget *f = QApplication::focusWidget(); f && dests_->isAncestorOf(f))
		focusKey = f->property("ldInputKey").toString();
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

	const QJsonObject doc = QJsonDocument::fromJson(QByteArray::fromStdString(j)).object();
	const QJsonArray inputs = doc.value("inputs").toArray();
	const QJsonArray dests = doc.value("dests").toArray();
	const QString muted = QStringLiteral("color: %1;").arg(pal_.muted.name());

	// Columns: dot, name, sparkline, state (two wide), Live/Stop.
	int row = 0;
	std::map<QString, QSpinBox *> spins;

	// The delay state of one input, in words, for the line that names it.
	auto delayState = [&](const QJsonObject &in) -> QString {
		if (!perInput_)
			return {};
		const bool delayed = in.value("mode").toString() == "delayed";
		const bool armed = in.value("armed").toBool();
		const int arm = int(in.value("arm_delay").toDouble(-1) + 0.5);
		const int active = int(in.value("active_delay").toDouble(-1) + 0.5);
		const int buffer = int(in.value("buffer_s").toDouble());
		const QString next = arm <= 0 ? QString() : armed ? T("Inputs.ArmedShort").arg(arm) : T("Inputs.Filling").arg(buffer).arg(arm);
		if (!delayed)
			return next;
		QString s = T("Inputs.DelayedShort").arg(active > 0 ? active : int(in.value("delay").toDouble() + 0.5));
		if (arm > 0 && active > 0 && arm != active)
			s += QStringLiteral(" → ") + next;
		return s;
	};

	// With delay_mode = per_input every input carries its own small copy of
	// the controls above — seconds and Arm, then Activate, Live, Live after and
	// Disarm — and the big ones are hidden, so nothing on screen claims to act
	// on everything.
	auto delayControls = [&](const QJsonObject &in) -> QWidget * {
		const QString qkey = in.value("key").toString();
		const std::string key = qkey.toStdString();
		if (!perInput_ || key.empty())
			return nullptr;
		const QString inputName = in.value("name").toString();
		const bool delayed = in.value("mode").toString() == "delayed";
		const bool armed = in.value("armed").toBool();
		const bool cutPending = in.value("cut_pending").toBool();
		const int arm = int(in.value("arm_delay").toDouble(-1) + 0.5);
		const int active = int(in.value("active_delay").toDouble(-1) + 0.5);
		const bool retime = delayed && arm > 0 && active > 0 && arm != active;
		int &want = inputDelay_[key];
		if (want <= 0)
			want = arm > 0 ? arm : delay_->value();
		// Armed from elsewhere — a hotkey, Stream Deck, the web panel: the box
		// follows, or it would offer to arm again the moment it was armed.
		int &seen = inputArmSeen_[key];
		if (arm > 0 && arm != seen)
			want = arm;
		seen = arm;

		auto *spin = new QSpinBox;
		spin->setRange(1, 3600);
		spin->setSuffix(QStringLiteral(" s"));
		spin->setValue(want);
		spin->setFixedWidth(70);
		spin->setToolTip(T("Inputs.DelayTip").arg(inputName));
		spin->setProperty("ldInputKey", qkey);
		spins[qkey] = spin;

		struct Mini {
			QPushButton *b;
			Icon icon;
			QColor color;
		};
		const std::string who = "?input=" + key;
		auto make = [&](const char *text, const char *tip, Icon icon, const QColor &c, const std::string &action) {
			auto *b = new QPushButton(T(text));
			b->setToolTip(T(tip).arg(inputName));
			b->setIconSize(QSize(12, 12));
			b->setCursor(Qt::PointingHandCursor);
			if (!action.empty())
				connect(b, &QPushButton::clicked, this, [this, action] { Act(action); });
			return Mini{b, icon, c};
		};
		const Mini armBtn = make("Dock.Arm", "Inputs.ArmOneTip", Icon::Arm, pal_.arm, std::string());
		const Mini actBtn = make(retime ? "Inputs.RetimeShort" : "Inputs.ActivateShort",
					 retime ? "Inputs.RetimeOneTip" : "Inputs.ActivateOneTip", Icon::Activate, pal_.tally,
					 "activate" + who);
		const Mini cutBtn = make("Dock.Cut", "Inputs.CutOneTip", Icon::Live, pal_.live, "cut" + who);
		const Mini afterBtn = make("Inputs.CutAfterShort", "Inputs.CutAfterOneTip", Icon::LiveAfter, pal_.live, "cut-after" + who);
		const Mini disBtn = make("Dock.Disarm", "Inputs.DisarmOneTip", Icon::Disarm, pal_.muted, "disarm" + who);
		connect(armBtn.b, &QPushButton::clicked, this,
			[this, spin, who] { Act("arm" + who + "&delay=" + std::to_string(spin->value())); });
		// A narrow dock keeps the icons; the words are in the tooltips.
		if (narrow_)
			for (const Mini &m : {actBtn, cutBtn, afterBtn, disBtn})
				m.b->setText(QString());
		actBtn.b->setEnabled(armed && (!delayed || retime));
		cutBtn.b->setEnabled(delayed);
		afterBtn.b->setEnabled(delayed && !cutPending);
		disBtn.b->setEnabled(arm > 0 && (!delayed || retime));

		// The same seconds cannot be armed twice; other seconds can, and that
		// is what turns Arm into "arm again". Whatever the input is heading
		// towards is the one filled button, as above.
		const Palette pal = pal_;
		auto show = [=](int seconds) {
			const bool rearm = (arm > 0 && arm != seconds) || (delayed && arm <= 0 && active > 0 && active != seconds);
			armBtn.b->setText(T(rearm ? "Dock.Rearm" : "Dock.Arm"));
			armBtn.b->setToolTip(T(rearm ? "Inputs.RearmOneTip" : "Inputs.ArmOneTip").arg(inputName));
			armBtn.b->setEnabled((!delayed && arm <= 0) || rearm);
			const QPushButton *primary = rearm ? armBtn.b
						     : retime ? actBtn.b
						     : delayed ? cutBtn.b
						     : arm > 0 ? actBtn.b
							       : armBtn.b;
			for (const Mini &m : {armBtn, actBtn, cutBtn, afterBtn, disBtn}) {
				const bool loud = m.b == primary && m.b->isEnabled();
				m.b->setStyleSheet((loud ? FilledButton(m.color) : GhostButton(m.color, pal.text)) +
						   QStringLiteral(" QPushButton { padding: 2px 6px; }"));
				m.b->setIcon(MakeIcon(m.icon, loud ? InkOn(m.color) : m.b->isEnabled() ? m.color : pal.muted));
			}
		};
		show(want);
		connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, [this, key, show](int v) {
			inputDelay_[key] = v;
			show(v);
		});

		auto *box = new QWidget;
		auto *lay = new QVBoxLayout(box);
		lay->setContentsMargins(0, 0, 0, 6);
		lay->setSpacing(4);
		auto *top = new QHBoxLayout;
		top->setSpacing(4);
		top->addWidget(spin);
		top->addWidget(armBtn.b, 1);
		auto *bottom = new QHBoxLayout;
		bottom->setSpacing(4);
		for (const Mini &m : {actBtn, cutBtn, afterBtn, disBtn})
			bottom->addWidget(m.b, 1);
		lay->addLayout(top);
		lay->addLayout(bottom);
		return box;
	};

	// One platform row. `index` is its place in the engine's list: that is what
	// dest/start and the OBS outputs are addressed by, whatever order it is shown
	// in. `own` is the input feeding only this platform, when its delay controls
	// belong under this row.
	auto addDest = [&](int index, bool indent, const QJsonObject &own) {
		if (outputsFolded_ && (!perInput_ || own.value("key").toString().isEmpty()))
			return;
		const QJsonObject d = dests.at(index).toObject();
		const std::string state = d.value("state").toString().toStdString();
		const bool on = d.value("on").toBool();
		const double kbps = d.value("kbps").toDouble();
		const QString error = d.value("error").toString();
		const QColor color = state == "live"	 ? pal_.live
				     : state == "error"	 ? pal_.tally
				     : state == "stopped" ? pal_.muted
							  : pal_.arm;
		auto *dot = new QLabel(QString::fromUtf8("●"));
		dot->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
		const QString destName = d.value("name").toString();
		auto *name = new QLabel(destName);
		if (indent)
			name->setContentsMargins(14, 0, 0, 0);
		QString st = T(("Dest." + state).c_str());
		if (kbps > 0)
			st += QString(" · %1 kb/s").arg(kbps, 0, 'f', 0);
		if (const QString ds = delayState(own); !ds.isEmpty())
			st += QStringLiteral(" · ") + ds;
		QLabel *stateLabel = nullptr;
		if (narrow_) {
			name->setTextFormat(Qt::RichText);
			name->setText(QStringLiteral("%1<br><span style='%2 font-size: 11px;'>%3</span>")
					      .arg(destName.toHtmlEscaped(), muted, st.toHtmlEscaped()));
			name->setToolTip(error);
		} else {
			stateLabel = new QLabel(st);
			stateLabel->setStyleSheet(muted);
			if (!error.isEmpty())
				stateLabel->setToolTip(error);
		}
		// Per-platform Start / Stop never shouts: the dock's loud button is
		// the delay action, not one of a handful of rows.
		QPushButton *toggle = nullptr;
		if (!outputsFolded_) {
			toggle = new QPushButton(on ? T("Dock.DestStop") : T("Dock.DestStart"));
			toggle->setToolTip(on ? T("Dock.DestStopTip") : T("Dock.DestStartTip"));
			toggle->setStyleSheet(GhostButton(on ? pal_.tally : pal_.live, pal_.text));
			toggle->setIcon(MakeIcon(on ? Icon::Stop : Icon::Play, on ? pal_.tally : pal_.live));
			toggle->setIconSize(QSize(12, 12));
			toggle->setCursor(Qt::PointingHandCursor);
			const std::string path = std::string(on ? "dest/stop" : "dest/start") + "?i=" + std::to_string(index);
			const bool turningOn = !on;
			connect(toggle, &QPushButton::clicked, this, [this, path, index, turningOn] {
				if (turningOn)
					OutputStartDest(size_t(index));
				else
					OutputStopDest(size_t(index));
				Act(path);
			});
		}
		destGrid_->addWidget(dot, row, 0);
		destGrid_->addWidget(name, row, 1);
		if (!narrow_ && !outputsFolded_)
			destGrid_->addWidget(new Sparkline(history(destName.toStdString(), state == "live" ? kbps : 0), color), row, 2);
		if (stateLabel)
			destGrid_->addWidget(stateLabel, row, 3, 1, 2);
		if (toggle)
			destGrid_->addWidget(toggle, row, 5);
		row++;
		if (QWidget *delay = delayControls(own)) {
			destGrid_->addWidget(delay, row, 1, 1, 5);
			row++;
		}
	};

	// What comes in: a shared input, or everything OBS sends taken as one line.
	// Neither of the on-air colours unless it is delayed or armed: bright when
	// there is signal, dim when not.
	auto addInput = [&](const QString &label, const QJsonObject &in, bool shared) {
		const bool signal = in.value("input").toBool();
		const double kbps = signal ? in.value("in_kbps").toDouble() : 0;
		const bool delayed = in.value("mode").toString() == "delayed";
		const bool armed = in.value("armed").toBool() || in.value("arm_delay").toDouble(-1) > 0;
		const QColor color = !shared ? (signal ? pal_.text : pal_.muted)
					     : delayed ? pal_.tally : armed ? pal_.arm : signal ? pal_.text : pal_.muted;
		auto *dot = new QLabel(QString::fromUtf8("●"));
		dot->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
		auto *name = new QLabel(label);
		if (shared)
			name->setStyleSheet(QStringLiteral("font-weight: 600;"));
		QString st = signal ? QString("%1 kb/s").arg(kbps, 0, 'f', 0) : T("Dock.NoSignalShort");
		if (shared) {
			if (const QString ds = delayState(in); !ds.isEmpty())
				st += QStringLiteral(" · ") + ds;
			else if (delayed)
				st += QStringLiteral(" · ") + T("Inputs.DelayedShort").arg(int(in.value("delay").toDouble() + 0.5));
		}
		QLabel *rate = nullptr;
		if (narrow_) {
			name->setTextFormat(Qt::RichText);
			name->setText(QStringLiteral("%1<br><span style='%2 font-size: 11px; font-weight: 400;'>%3</span>")
					      .arg(label.toHtmlEscaped(), muted, st.toHtmlEscaped()));
		} else {
			rate = new QLabel(st);
			rate->setStyleSheet(muted);
		}
		destGrid_->addWidget(dot, row, 0);
		destGrid_->addWidget(name, row, 1);
		if (!narrow_)
			destGrid_->addWidget(new Sparkline(history("in:" + in.value("key").toString().toStdString(), kbps), color), row, 2);
		// Each picture has its own preview: what comes in on this input and
		// what goes out of it, with its own delay.
		QPushButton *look = nullptr;
		if (inputs.size() > 1 && !in.value("name").toString().isEmpty()) {
			look = new QPushButton(QString::fromUtf8("▶"));
			look->setFlat(true);
			look->setCursor(Qt::PointingHandCursor);
			look->setFixedWidth(26);
			look->setToolTip(T("Dock.PreviewInputTip").arg(label));
			const QString inputName = in.value("name").toString();
			connect(look, &QPushButton::clicked, this, [inputName] {
				QUrl url(QString::fromStdString(LivePreviewUrl()));
				QUrlQuery q(url);
				q.addQueryItem(QStringLiteral("input"), inputName);
				url.setQuery(q);
				QDesktopServices::openUrl(url);
			});
		}
		if (rate)
			destGrid_->addWidget(rate, row, 3, 1, look ? 2 : 3);
		if (look)
			destGrid_->addWidget(look, row, 5);
		row++;
		if (QWidget *delay = shared ? delayControls(in) : nullptr) {
			destGrid_->addWidget(delay, row, 1, 1, 5);
			row++;
		}
	};

	// A platform's own input is not shown as an input: it is that platform's
	// row. The settings say which ones those are; an engine still running on
	// an older reading of the file is judged by the same rule the file is.
	auto isOwn = [&](const QJsonObject &in) {
		const QString name = in.value("name").toString();
		if (ownInputs_.count(name.toStdString()))
			return true;
		if (sharedInputs_.count(name.toStdString()))
			return false;
		int fed = 0;
		bool same = false;
		for (const QJsonValue &d : dests)
			if (d.toObject().value("input").toString() == name) {
				++fed;
				same = d.toObject().value("name").toString() == name;
			}
		return fed == 1 && same;
	};

	bool anyShared = false;
	for (const QJsonValue &v : inputs)
		anyShared = anyShared || !isOwn(v.toObject());

	// The outputs fold away: in a short dock the inputs and their delay matter
	// more than a list of platforms that are simply running.
	if (!dests.isEmpty()) {
		int live = 0;
		for (const QJsonValue &d : dests)
			live += d.toObject().value("state").toString() == "live" ? 1 : 0;
		auto *fold = new QPushButton((outputsFolded_ ? QStringLiteral("▸ ") : QStringLiteral("▾ ")) +
					     T("Dock.OutputsFold").arg(live).arg(dests.size()));
		fold->setCursor(Qt::PointingHandCursor);
		fold->setToolTip(T("Dock.OutputsFoldTip"));
		fold->setStyleSheet(QStringLiteral("QPushButton { text-align: left; border: none; background: transparent;"
						   " padding: 0; color: %1; font-weight: 600; }"
						   "QPushButton:hover { color: %2; }")
					    .arg(pal_.muted.name(), pal_.text.name()));
		connect(fold, &QPushButton::clicked, this, [this] {
			outputsFolded_ = !outputsFolded_;
			QSettings(QStringLiteral("skullboypl"), QStringLiteral("livedelay-obs"))
				.setValue(QStringLiteral("dock/outputsFolded"), outputsFolded_);
			// The button just clicked is one of the rows: redraw once the click is over.
			QTimer::singleShot(0, this, [this] {
				if (!last_.empty())
					ShowDestinations(std::string(last_));
			});
		});
		destGrid_->addWidget(fold, row, 0, 1, 6);
		row++;
	}

	// Every platform encoding for itself is the picture the dock always had:
	// one line for what OBS sends, then the platforms.
	if (!anyShared) {
		QJsonObject whole;
		whole.insert("input", doc.value("input"));
		whole.insert("in_kbps", doc.value("in_kbps"));
		addInput(T("Dock.InputRow"), whole, false);
	}
	std::vector<bool> shown(size_t(dests.size()), false);
	for (const QJsonValue &v : inputs) {
		const QJsonObject in = v.toObject();
		const QString name = in.value("name").toString();
		const bool own = isOwn(in);
		if (!own)
			addInput(name, in, true);
		for (int n = 0; n < dests.size(); ++n)
			if (dests.at(n).toObject().value("input").toString() == name) {
				addDest(n, !own, own ? in : QJsonObject());
				shown[size_t(n)] = true;
			}
	}
	// An engine too old to report inputs, or a platform naming one that is
	// not there: the row still has to be on screen.
	for (int n = 0; n < dests.size(); ++n)
		if (!shown[size_t(n)])
			addDest(n, false, QJsonObject());
	if (dests.isEmpty()) {
		auto *none = new QLabel(T("Dock.NoPlatforms"));
		none->setWordWrap(true);
		none->setStyleSheet(muted);
		destGrid_->addWidget(none, row, 0, 1, 6);
	}
	// The rows were rebuilt under whoever was typing seconds: give the box back.
	if (auto it = spins.find(focusKey); !focusKey.isEmpty() && it != spins.end())
		it->second->setFocus();
	// What is on screen now may no longer fit the width it was drawn for.
	if (!fitting_)
		QTimer::singleShot(0, this, [this] { FitWidth(); });
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
		delay_->blockSignals(true);
		delay_->setValue(int(arm_delay > 0 ? arm_delay : JsonNumber(j, "default_delay", 15)));
		delay_->blockSignals(false);
	}
	// The armed delay that is on air. Arming another value while delayed does
	// not change it until Activate is pressed again.
	const double active = JsonNumber(j, "active_delay", -1);
	const int armS = int(arm_delay + 0.5), activeS = int(active + 0.5), wantS = delay_->value();
	const bool retime = delayed && arm_delay > 0 && active > 0 && armS != activeS;
	const bool rearm = (arm_delay > 0 && armS != wantS) || (delayed && arm_delay <= 0 && active > 0 && activeS != wantS);

	// Show configured Arm seconds, not measured keyframe lag (always a bit higher).
	const double shown_delay = active > 0 ? active : arm_delay > 0 ? arm_delay : delay;
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

	const bool filling = input && arm_delay > 0 && !armed && (!delayed || retime);
	fill_->setVisible(filling && !perInput_);
	if (filling) {
		fill_->setRange(0, int(arm_delay * 10));
		fill_->setValue(int(buffer * 10));
	}

	QString d;
	if (!input)
		d = T(delayed ? "Dock.DrainingTail" : "Dock.WaitingHint");
	if (cut_pending)
		d += T("Dock.CutPending");
	if (retime && !perInput_)
		d += armed ? T("Dock.RetimeReady").arg(armS) : T("Dock.RetimeFilling").arg(buffer, 0, 'f', 0).arg(armS);
	detail_->setText(d);
	detail_->setVisible(!d.isEmpty() && !compact_);
	status_->setToolTip(compact_ ? d : QString());
	ShowDestinations(j);

	// The same seconds cannot be armed twice; other seconds can, at any time —
	// also while the delay is on, which is how it is made longer or shorter
	// without going live in between.
	arm_->setText(T(rearm ? "Dock.Rearm" : "Dock.Arm"));
	arm_->setToolTip(T(rearm ? "Dock.RearmTip" : "Dock.ArmTip"));
	arm_->setEnabled((!delayed && arm_delay <= 0) || rearm);
	activate_->setText(retime ? (narrow_ ? T("Inputs.RetimeShort") : T("Dock.Retime").arg(armS))
				  : T(narrow_ ? "Inputs.ActivateShort" : "Dock.Activate"));
	activate_->setToolTip(retime ? T("Dock.RetimeTip").arg(armS) : T("Dock.ActivateTip"));
	activate_->setEnabled(armed && (!delayed || retime));
	cut_->setEnabled(delayed);
	cut_after_->setEnabled(delayed && !cut_pending);
	disarm_->setEnabled(arm_delay > 0 && (!delayed || retime));
	// With a delay per input every input has these on its own row, and one big
	// set acting on all of them at once would only mislead.
	for (QWidget *w : std::initializer_list<QWidget *>{delay_, arm_, activate_, cut_, cut_after_, disarm_})
		w->setVisible(!perInput_);
	// Whatever this state is heading towards is the one loud button.
	SetPrimary(rearm ? arm_ : retime ? activate_ : delayed ? cut_ : arm_delay > 0 ? activate_ : arm_);
}

} // namespace livedelay
