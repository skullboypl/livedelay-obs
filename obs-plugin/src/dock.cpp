#include "dock.hpp"
#include "client.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <windows.h>
#include <shellapi.h>

#include <thread>

namespace livedelay {

std::wstring ProxyExePath()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&ProxyExePath), &self);
	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
	std::wstring p(path, n);
	return p.substr(0, p.find_last_of(L"\\/") + 1) + L"livedelay.exe";
}

static QPushButton *Button(const char *text, const char *tip)
{
	auto *b = new QPushButton(QString::fromUtf8(text));
	b->setToolTip(QString::fromUtf8(tip));
	b->setMinimumHeight(30);
	return b;
}

Dock::Dock(QWidget *parent) : QWidget(parent)
{
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
	delay_->setToolTip(QString::fromUtf8("Opóźnienie, jakie ustawi Arm"));
	arm_ = Button("Arm", "Zacznij trzymać bufor na wybrane opóźnienie");
	activate_ = Button("Activate", "Włącz opóźnienie: widzowie cofną się do klatki sprzed N s");
	cut_ = Button("Cut", "Wróć na żywo teraz");
	cut_after_ = Button("Cut po wyemitowaniu", "Wróć na żywo, gdy obecna chwila dotrze do widzów");
	disarm_ = Button("Rozbrój", "Przestań trzymać bufor");
	start_ = Button("Uruchom LiveDelay", "Uruchom proxy LiveDelay");
	fill_ = new QProgressBar;
	fill_->setTextVisible(false);
	fill_->setMaximumHeight(6);
	fill_->hide();

	auto *armRow = new QHBoxLayout;
	armRow->addWidget(delay_);
	armRow->addWidget(arm_, 1);
	auto *mainRow = new QHBoxLayout;
	mainRow->addWidget(activate_);
	mainRow->addWidget(cut_);
	auto *moreRow = new QHBoxLayout;
	moreRow->addWidget(cut_after_);
	moreRow->addWidget(disarm_);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(status_);
	layout->addWidget(fill_);
	layout->addWidget(detail_);
	layout->addLayout(armRow);
	layout->addLayout(mainRow);
	layout->addLayout(moreRow);
	layout->addWidget(start_);
	layout->addWidget(error_);
	layout->addStretch();

	connect(arm_, &QPushButton::clicked, this, [this] { Hotkey("arm"); });
	connect(activate_, &QPushButton::clicked, this, [this] { Act("activate"); });
	connect(cut_, &QPushButton::clicked, this, [this] { Act("cut"); });
	connect(cut_after_, &QPushButton::clicked, this, [this] { Act("cut-after"); });
	connect(disarm_, &QPushButton::clicked, this, [this] { Act("disarm"); });
	connect(start_, &QPushButton::clicked, this, [this] { StartProxy(); });

	ShowOffline();
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
		QString msg;
		if (status == 0)
			msg = QString::fromUtf8("LiveDelay nie odpowiada.");
		else if (status != 200)
			msg = QString::fromStdString(JsonString(body, "error"));
		QMetaObject::invokeMethod(
			self, [self, msg] { if (self) self->ShowError(msg); }, Qt::QueuedConnection);
	}).detach();
}

void Dock::Hotkey(const std::string &action)
{
	Act(action == "arm" ? "arm?delay=" + std::to_string(delay_->value()) : action);
}

void Dock::StartProxy()
{
	std::wstring exe = ProxyExePath();
	if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
		ShowError(QString::fromUtf8("Nie znaleziono ") + QString::fromStdWString(exe));
		return;
	}
	ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	ShowError({});
}

void Dock::ShowError(const QString &msg)
{
	error_->setText(msg);
	error_->setVisible(!msg.isEmpty());
}

static void Pill(QLabel *l, const QString &text, const char *bg)
{
	l->setText(text);
	l->setStyleSheet(QString("font-size: 15px; font-weight: 600; border-radius: 6px; color: white; background: %1;").arg(bg));
}

void Dock::ShowOffline()
{
	Pill(status_, QString::fromUtf8("LiveDelay nie działa"), "#5a5f6b");
	detail_->setText(QString::fromUtf8("Uruchom proxy, żeby sterować opóźnieniem."));
	fill_->hide();
	for (QPushButton *b : {arm_, activate_, cut_, cut_after_, disarm_})
		b->setEnabled(false);
	start_->show();
}

void Dock::ShowState(const std::string &j)
{
	start_->hide();
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
		Pill(status_, QString::fromUtf8("OPÓŹNIENIE %1 s").arg(delay, 0, 'f', 0), "#c62828");
	else if (armed)
		Pill(status_, QString::fromUtf8("NA ŻYWO · uzbrojone %1 s").arg(arm_delay, 0, 'f', 0), "#b26a00");
	else if (arm_delay > 0)
		Pill(status_, QString::fromUtf8("NA ŻYWO · bufor %1/%2 s").arg(buffer, 0, 'f', 0).arg(arm_delay, 0, 'f', 0), "#b26a00");
	else
		Pill(status_, QString::fromUtf8("NA ŻYWO"), "#2e7d32");

	const bool filling = !delayed && arm_delay > 0 && !armed;
	fill_->setVisible(filling);
	if (filling) {
		fill_->setRange(0, int(arm_delay * 10));
		fill_->setValue(int(buffer * 10));
	}

	QString d = input ? QString() : QString::fromUtf8("Brak sygnału z OBS. ");
	if (cut_pending)
		d += QString::fromUtf8("Cut czeka, aż ta chwila dotrze do widzów. ");
	size_t p = 0;
	int dests = 0;
	while ((p = j.find("\"name\":\"", p)) != std::string::npos) {
		std::string name = JsonString(j, "name", p);
		std::string state = JsonString(j, "state", p);
		double kbps = 0;
		size_t k = j.find("\"kbps\":", p);
		if (k != std::string::npos)
			kbps = std::strtod(j.c_str() + k + 7, nullptr);
		d += QString::fromStdString(name) + ": " + QString::fromStdString(state);
		if (kbps > 0)
			d += QString(" %1 kb/s").arg(kbps, 0, 'f', 0);
		d += "   ";
		p += 8;
		dests++;
	}
	if (dests == 0)
		d += QString::fromUtf8("Brak celów: dodaj platformę w livedelay.ini (menu w trayu).");
	detail_->setText(d.trimmed());

	arm_->setEnabled(!delayed);
	activate_->setEnabled(!delayed && armed);
	cut_->setEnabled(delayed);
	cut_after_->setEnabled(delayed && !cut_pending);
	disarm_->setEnabled(!delayed && arm_delay > 0);
}

} // namespace livedelay
