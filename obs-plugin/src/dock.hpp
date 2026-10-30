#pragma once

#include "settings.hpp"
#include "theme.hpp"

#include <QWidget>

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <string>

class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QProgressBar;
class QSpinBox;

namespace livedelay {

class EventStream;
constexpr size_t kHistory = 60; // seconds of bitrate shown per row

// The LiveDelay panel: state at a glance plus Arm / Activate / Cut.
// Redraws only when the proxy pushes a new state; no polling.
class Dock : public QWidget {
public:
	explicit Dock(QWidget *parent = nullptr);
	~Dock() override;

	// Runs an action on a worker thread; errors show up in the panel.
	void Act(const std::string &action);
	// Arm takes the delay set in the panel; other actions go straight through.
	void Hotkey(const std::string &action);
	// Re-reads every text after the language changed.
	void Retranslate();
	// Picks up livedelay.ini again: the web panel, the standalone app and the
	// settings window all write it while the dock is open.
	void ReloadConfig();

private:
	void ApplyTheme();
	void SetPrimary(QPushButton *primary);
	void ShowState(const std::string &json);
	void ShowOffline();
	void ShowError(const QString &msg);
	void ShowDestinations(const std::string &json);
	void UpdateBanner();

	QFrame *banner_;
	QFrame *limits_;
	QLabel *limitsText_;
	QPushButton *limitsFit_;
	QPushButton *limitsHide_;
	QFrame *dests_;
	QGridLayout *destGrid_;
	QLabel *status_;
	QLabel *detail_;
	QLabel *error_;
	QSpinBox *delay_;
	QProgressBar *fill_;
	QPushButton *arm_;
	QPushButton *activate_;
	QPushButton *cut_;
	QPushButton *cut_after_;
	QPushButton *disarm_;
	QPushButton *start_;
	bool delay_set_ = false;
	QString limitsKey_; // what the limits banner is about right now
	QString limitsAck_; // the same, as last dismissed; kept in livedelay.ini
	DockTheme theme_ = DockTheme::Obs;
	Palette pal_ = ThemePalette(DockTheme::Obs);
	unsigned long long configStamp_ = 0; // livedelay.ini write time we last read
	std::string language_;
	std::string engine_text_; // ini parts that only a proxy restart picks up
	std::string last_; // last state JSON, empty while offline
	std::map<std::string, std::deque<double>> history_;
	std::chrono::steady_clock::time_point lastSample_{};
	std::unique_ptr<EventStream> events_;
};

// Last error reported by the embedded proxy, empty if none.
std::string LastError();

} // namespace livedelay
