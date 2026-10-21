#pragma once

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

private:
	void ShowState(const std::string &json);
	void ShowOffline();
	void ShowError(const QString &msg);
	void ShowDestinations(const std::string &json);
	void UpdateBanner();

	QFrame *banner_;
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
	std::string last_; // last state JSON, empty while offline
	std::map<std::string, std::deque<double>> history_;
	std::chrono::steady_clock::time_point lastSample_{};
	std::unique_ptr<EventStream> events_;
};

// Last error reported by the embedded proxy, empty if none.
std::string LastError();

} // namespace livedelay
