#include "wizard.hpp"
#include "client.hpp"
#include "i18n.hpp"
#include "outputs.hpp"
#include "settings.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

namespace livedelay {

bool AitumVerticalLoaded()
{
	// "vertical-canvas" is Aitum Vertical's module (buildspec.json in
	// Aitum/obs-vertical-canvas). Aitum's Stream Suite carries the same canvas.
	return obs_get_module("vertical-canvas") != nullptr || obs_get_module("aitum-stream-suite") != nullptr;
}

namespace {

constexpr const char *kAitumUrl = "https://github.com/Aitum/obs-vertical-canvas/releases";
constexpr int kSteps = 5;

QString Q(const std::string &s)
{
	return QString::fromStdString(s);
}

// Secondary text: the window's own text colour, a step back. A palette role
// such as placeholder-text is close to invisible on OBS's dark themes.
QString Quiet()
{
	QColor c = qApp->palette().color(QPalette::WindowText);
	return QStringLiteral("color: rgba(%1, %2, %3, 0.72);").arg(c.red()).arg(c.green()).arg(c.blue());
}

QLabel *Text(const QString &t, bool muted = false)
{
	auto *l = new QLabel(t);
	l->setWordWrap(true);
	l->setTextFormat(Qt::RichText);
	l->setOpenExternalLinks(true);
	if (muted)
		l->setStyleSheet(Quiet());
	return l;
}

QLabel *Title(const QString &t)
{
	auto *l = new QLabel(t);
	l->setWordWrap(true);
	l->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600;"));
	return l;
}

// A read-only line with a copy button: what the user pastes elsewhere.
QWidget *CopyRow(QLineEdit **out)
{
	auto *w = new QWidget;
	auto *lay = new QHBoxLayout(w);
	lay->setContentsMargins(0, 0, 0, 0);
	auto *line = new QLineEdit;
	line->setReadOnly(true);
	auto *copy = new QPushButton(T("Settings.Copy"));
	QObject::connect(copy, &QPushButton::clicked, line, [line] { QApplication::clipboard()->setText(line->text()); });
	lay->addWidget(line, 1);
	lay->addWidget(copy);
	*out = line;
	return w;
}

class VerticalWizard : public QDialog {
public:
	explicit VerticalWizard(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(T("Wizard.Title"));
		setMinimumWidth(560);

		pages_ = new QStackedWidget;
		pages_->addWidget(PageSource());
		pages_->addWidget(PageInput());
		pages_->addWidget(PagePaste());
		pages_->addWidget(PageSignal());
		pages_->addWidget(PagePlatforms());

		step_ = new QLabel;
		step_->setStyleSheet(Quiet());
		back_ = new QPushButton(T("Wizard.Back"));
		next_ = new QPushButton(T("Wizard.Next"));
		next_->setDefault(true);
		auto *close = new QPushButton(T("Wizard.Close"));
		auto *buttons = new QHBoxLayout;
		buttons->addWidget(step_, 1);
		buttons->addWidget(close);
		buttons->addWidget(back_);
		buttons->addWidget(next_);

		auto *lay = new QVBoxLayout(this);
		lay->setContentsMargins(16, 16, 16, 12);
		lay->addWidget(pages_, 1);
		lay->addLayout(buttons);

		connect(close, &QPushButton::clicked, this, &QDialog::reject);
		connect(back_, &QPushButton::clicked, this, [this] { Go(pages_->currentIndex() - 1); });
		connect(next_, &QPushButton::clicked, this, [this] {
			if (pages_->currentIndex() == kSteps - 1) {
				if (Apply())
					accept();
				return;
			}
			Go(pages_->currentIndex() + 1);
		});

		poll_ = new QTimer(this);
		poll_->setInterval(1000);
		connect(poll_, &QTimer::timeout, this, [this] { Poll(); });

		FindExisting();
		Go(0);
	}

private:
	// ---- pages -------------------------------------------------------------

	QWidget *PageSource()
	{
		auto *p = new QWidget;
		auto *l = new QVBoxLayout(p);
		l->addWidget(Title(T("Wizard.SourceTitle")));
		l->addWidget(Text(T("Wizard.SourceBody")));
		const bool aitum = AitumVerticalLoaded();
		auto *found = Text(aitum ? T("Wizard.AitumFound") : T("Wizard.AitumMissing").arg(kAitumUrl));
		found->setStyleSheet(aitum ? QStringLiteral("color: #3ddc84; font-weight: 600;")
					   : QStringLiteral("color: #ffbf3c;"));
		l->addWidget(found);
		l->addWidget(Text(T("Wizard.SourceOther"), true));
		l->addStretch();
		return p;
	}

	QWidget *PageInput()
	{
		auto *p = new QWidget;
		auto *l = new QVBoxLayout(p);
		l->addWidget(Title(T("Wizard.InputTitle")));
		l->addWidget(Text(T("Wizard.InputBody")));
		make_ = new QPushButton(T("Wizard.Make"));
		make_->setToolTip(T("Wizard.MakeTip"));
		l->addWidget(make_, 0, Qt::AlignLeft);
		inputState_ = Text(QString());
		l->addWidget(inputState_);
		buffer_ = Text(QString(), true);
		l->addWidget(buffer_);
		l->addStretch();
		connect(make_, &QPushButton::clicked, this, [this] { Make(); });
		return p;
	}

	QWidget *PagePaste()
	{
		auto *p = new QWidget;
		auto *l = new QVBoxLayout(p);
		l->addWidget(Title(T("Wizard.PasteTitle")));
		l->addWidget(Text(T("Wizard.PasteBody")));
		auto *form = new QFormLayout;
		form->addRow(T("Wizard.Server"), CopyRow(&server_));
		form->addRow(T("Wizard.Key"), CopyRow(&key_));
		l->addLayout(form);
		l->addWidget(Text(T("Wizard.PasteNotPlatform"), true));
		l->addStretch();
		return p;
	}

	QWidget *PageSignal()
	{
		auto *p = new QWidget;
		auto *l = new QVBoxLayout(p);
		l->addWidget(Title(T("Wizard.SignalTitle")));
		l->addWidget(Text(T("Wizard.SignalBody")));
		signal_ = new QLabel;
		signal_->setWordWrap(true);
		l->addWidget(signal_);
		shape_ = Text(QString());
		l->addWidget(shape_);
		checks_ = Text(T("Wizard.SignalChecks"), true);
		l->addWidget(checks_);
		l->addStretch();
		return p;
	}

	QWidget *PagePlatforms()
	{
		auto *p = new QWidget;
		auto *l = new QVBoxLayout(p);
		l->addWidget(Title(T("Wizard.PlatformsTitle")));
		l->addWidget(Text(T("Wizard.PlatformsBody")));
		boxes_ = new QWidget;
		boxesLay_ = new QVBoxLayout(boxes_);
		boxesLay_->setContentsMargins(0, 0, 0, 0);
		l->addWidget(boxes_);
		auto *addTikTok = new QPushButton(T("Wizard.AddTikTok"));
		addTikTok->setToolTip(T("Wizard.AddTikTokTip"));
		l->addWidget(addTikTok, 0, Qt::AlignLeft);
		l->addWidget(Text(T("Wizard.TikTokKey"), true));
		summary_ = Text(QString(), true);
		l->addWidget(summary_);
		l->addStretch();
		connect(addTikTok, &QPushButton::clicked, this, [this] {
			wantTikTok_ = true;
			FillBoxes();
		});
		return p;
	}

	// ---- flow --------------------------------------------------------------

	void Go(int page)
	{
		if (page < 0 || page >= kSteps)
			return;
		pages_->setCurrentIndex(page);
		step_->setText(T("Wizard.Step").arg(page + 1).arg(kSteps));
		back_->setEnabled(page > 0);
		const bool last = page == kSteps - 1;
		next_->setText(last ? T("Wizard.Finish") : T("Wizard.Next"));
		// Step two is the only one that can hold you back: without an input
		// there is nothing to paste and nothing to wait for.
		next_->setEnabled(page != 1 || !key_->text().isEmpty());
		if (page == 1)
			ShowInput();
		if (page == 3) {
			Poll();
			poll_->start();
		} else {
			poll_->stop();
		}
		if (last)
			FillBoxes();
	}

	// An input another program already feeds is the one to reuse: running the
	// wizard twice must not make a second.
	void FindExisting()
	{
		const Settings s = Settings::Load(ConfigPath());
		for (const Input &in : s.inputs)
			if (in.external) {
				name_ = in.name;
				key_->setText(Q(in.key));
				break;
			}
		server_->setText(QStringLiteral("rtmp://127.0.0.1:%1/live").arg(s.RtmpPort()));
	}

	void ShowInput()
	{
		const bool have = !key_->text().isEmpty();
		make_->setVisible(!have);
		inputState_->setText(have ? T("Wizard.InputReady").arg(Q(name_), key_->text()) : T("Wizard.InputNone"));
		inputState_->setStyleSheet(have ? QStringLiteral("color: #3ddc84; font-weight: 600;") : QString());
		const Settings s = Settings::Load(ConfigPath());
		const size_t n = std::max<size_t>(1, s.EffectiveInputs().size() + (have ? 0 : 1));
		buffer_->setText(T("Wizard.BufferSplit").arg(s.max_buffer_mb).arg(qulonglong(n)).arg(s.max_buffer_mb / int(n)));
		next_->setEnabled(have);
	}

	bool ConfirmRestart()
	{
		if (!obs_frontend_streaming_active())
			return true;
		return QMessageBox::question(this, QStringLiteral("LiveDelay"), T("Settings.RestartWarning")) == QMessageBox::Yes;
	}

	// The proxy owns one slot per input, so a new input or a moved platform
	// means starting it again with the file as it now is.
	bool SaveAndRestart(const Settings &s)
	{
		if (!s.Save(ConfigPath())) {
			QMessageBox::warning(this, QStringLiteral("LiveDelay"), T("Settings.SaveFailed"));
			return false;
		}
		const std::string err = StartProxy();
		if (!err.empty())
			QMessageBox::warning(this, QStringLiteral("LiveDelay"), Q(err));
		OutputsReloadConfig();
		if (OnConfigChanged)
			OnConfigChanged();
		return true;
	}

	void Make()
	{
		if (!ConfirmRestart())
			return;
		Settings s = Settings::Load(ConfigPath());
		// A file without inputs runs on one per platform, made up by the
		// engine. Write those down first, or the new input would be the only
		// one and every platform would point at nothing.
		if (s.inputs.empty()) {
			s.inputs = s.EffectiveInputs();
			for (size_t n = 0; n < s.dests.size() && n < s.inputs.size(); ++n)
				s.dests[n].input = s.inputs[n].name;
		}
		Input in;
		in.name = S(T("Wizard.InputName"));
		for (int n = 2; s.FindInput(in.name); ++n)
			in.name = S(T("Wizard.InputName")) + " " + std::to_string(n);
		in.key = FreeIngestKey(s);
		in.external = true;
		s.inputs.push_back(in);
		if (!SaveAndRestart(s))
			return;
		blog(LOG_INFO, "[livedelay] wizard: input %s on %s, fed by another program", in.name.c_str(), in.key.c_str());
		name_ = in.name;
		key_->setText(Q(in.key));
		ShowInput();
	}

	void Poll()
	{
		int status = 0;
		const std::string body = Request("GET", "/state", &status);
		QJsonObject found;
		const QJsonArray inputs = QJsonDocument::fromJson(QByteArray::fromStdString(body)).object().value("inputs").toArray();
		for (const QJsonValue &v : inputs)
			if (v.toObject().value("key").toString() == key_->text())
				found = v.toObject();
		const bool live = found.value("input").toBool();
		const int w = found.value("width").toInt(), h = found.value("height").toInt();
		const double kbps = found.value("in_kbps").toDouble();
		if (!live) {
			signal_->setText(T("Wizard.SignalNone"));
			signal_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600;"));
			shape_->clear();
			checks_->show();
			return;
		}
		const QString size = w > 0 && h > 0 ? QStringLiteral("%1×%2").arg(w).arg(h) : T("Wizard.SignalPicture");
		signal_->setText(T("Wizard.SignalSeen").arg(size).arg(kbps, 0, 'f', 0));
		signal_->setStyleSheet(QStringLiteral("color: #3ddc84; font-size: 14px; font-weight: 600;"));
		checks_->hide();
		if (w <= 0 || h <= 0) {
			shape_->setText(T("Wizard.ShapeUnknown"));
			shape_->setStyleSheet(QString());
		} else if (h > w) {
			shape_->setText(T("Wizard.ShapeVertical"));
			shape_->setStyleSheet(QStringLiteral("color: #3ddc84;"));
		} else {
			shape_->setText(T("Wizard.ShapeHorizontal"));
			shape_->setStyleSheet(QStringLiteral("color: #ffbf3c;"));
		}
	}

	void FillBoxes()
	{
		// Redrawn when TikTok is added: keep what was already ticked.
		std::vector<std::pair<QString, bool>> kept;
		for (QCheckBox *box : boxes_list_)
			kept.emplace_back(box->text(), box->isChecked());
		while (QLayoutItem *item = boxesLay_->takeAt(0)) {
			delete item->widget();
			delete item;
		}
		boxes_list_.clear();
		tiktok_ = nullptr;
		const Settings s = Settings::Load(ConfigPath());
		for (const Destination &d : s.dests) {
			auto *box = new QCheckBox(Q(d.name));
			box->setChecked(d.input == name_);
			for (const auto &k : kept)
				if (k.first == box->text())
					box->setChecked(k.second);
			if (const Input *from = s.FindInput(d.input); from && d.input != name_)
				box->setToolTip(from->own ? T("Inputs.TakesOwn") : T("Inputs.TakesFrom").arg(Q(from->name)));
			boxesLay_->addWidget(box);
			boxes_list_.push_back(box);
		}
		if (wantTikTok_) {
			auto *box = new QCheckBox(T("Wizard.NewTikTok"));
			box->setChecked(true);
			boxesLay_->addWidget(box);
			tiktok_ = box;
		}
		summary_->setText(T("Wizard.Summary").arg(Q(name_), key_->text()));
	}

	bool Apply()
	{
		Settings s = Settings::Load(ConfigPath());
		const Input *mine = s.FindInput(name_);
		if (!mine)
			return true; // nothing was made; nothing to point anywhere
		bool changed = false;
		for (size_t n = 0; n < s.dests.size() && n < boxes_list_.size(); ++n) {
			Destination &d = s.dests[n];
			const bool on = boxes_list_[n]->isChecked();
			if (on && d.input != name_) {
				// The encode it had for itself has nobody left to feed.
				const std::string old = d.input;
				d.input = name_;
				for (size_t i = 0; i < s.inputs.size(); ++i)
					if (s.inputs[i].name == old && s.inputs[i].own) {
						s.inputs.erase(s.inputs.begin() + i);
						break;
					}
				changed = true;
			} else if (!on && d.input == name_) {
				// Unticked: back to an encode of its own, as before the wizard.
				Input own;
				own.own = true;
				own.name = d.name;
				for (int k = 2; s.FindInput(own.name); ++k)
					own.name = d.name + " " + std::to_string(k);
				own.key = FreeIngestKey(s);
				s.inputs.push_back(own);
				d.input = own.name;
				changed = true;
			}
		}
		if (tiktok_ && tiktok_->isChecked()) {
			Destination d;
			d.name = "TikTok";
			for (int n = 2;; ++n) {
				bool taken = false;
				for (const Destination &e : s.dests)
					taken = taken || e.name == d.name;
				if (!taken)
					break;
				d.name = "TikTok " + std::to_string(n);
			}
			d.encode_mode = EncodeMode::Obs;
			d.input = name_;
			d.autostart = true;
			s.dests.push_back(d);
			changed = true;
		}
		for (Destination &d : s.dests)
			if (const Input *in = s.FindInput(d.input))
				d.ingest_key = in->key;
		if (!changed)
			return true;
		if (!ConfirmRestart() || !SaveAndRestart(s))
			return false;
		if (tiktok_ && tiktok_->isChecked())
			QMessageBox::information(this, QStringLiteral("LiveDelay"), T("Wizard.TikTokFill"));
		return true;
	}

	static std::string S(const QString &s)
	{
		return s.trimmed().toStdString();
	}

	QStackedWidget *pages_ = nullptr;
	QLabel *step_ = nullptr;
	QPushButton *back_ = nullptr, *next_ = nullptr, *make_ = nullptr;
	QLabel *inputState_ = nullptr, *buffer_ = nullptr;
	QLineEdit *server_ = nullptr, *key_ = nullptr;
	QLabel *signal_ = nullptr, *shape_ = nullptr, *checks_ = nullptr, *summary_ = nullptr;
	QWidget *boxes_ = nullptr;
	QVBoxLayout *boxesLay_ = nullptr;
	std::vector<QCheckBox *> boxes_list_;
	QCheckBox *tiktok_ = nullptr;
	bool wantTikTok_ = false;
	QTimer *poll_ = nullptr;
	std::string name_;
};

} // namespace

void ShowVerticalWizard(QWidget *parent)
{
	VerticalWizard w(parent);
	w.exec();
}

} // namespace livedelay
