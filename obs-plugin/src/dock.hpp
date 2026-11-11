#pragma once

#include "settings.hpp"
#include "theme.hpp"

#include <QMargins>
#include <QWidget>

#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

class QFrame;
class QGridLayout;
class QLabel;
class QPushButton;
class QProgressBar;
class QResizeEvent;
class QScrollArea;
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
	void ReadInputs(const Settings &s);
	void Reflow();
	void FitWidth();
	void ApplyDensity();
	void resizeEvent(QResizeEvent *event) override;

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
	bool obsLive_ = false;
	bool perInput_ = false; // delay_mode = per_input: a delay button on each input
	std::set<std::string> ownInputs_, sharedInputs_; // by name, from the ini
	std::map<std::string, int> inputDelay_;           // seconds typed per input key
	std::map<std::string, int> inputArmSeen_;         // armed seconds last drawn, per input key
	QGridLayout *bigGrid_ = nullptr;   // Activate, Live, Live after, Disarm
	QGridLayout *toolsGrid_ = nullptr; // Settings, Panel, Preview, Help
	std::vector<QPushButton *> tools_;
	bool narrow_ = false;        // the wide form does not fit: one column, icons, no bitrate lines
	int wideNeeds_ = 0;          // width the wide form asked for when it last overflowed
	bool fitting_ = false;       // FitWidth is redrawing; do not start another round
	bool compact_ = false;       // the roomy form does not fit the height: tighter everything
	int roomyNeeds_ = 0;         // height the roomy form asked for when it last overflowed
	QMargins normalMargins_;
	int normalSpacing_ = -1;
	QWidget *body_ = nullptr;
	QScrollArea *scroll_ = nullptr;
	bool outputsFolded_ = false; // platform rows hidden; remembered across sessions
	bool rowsDeferred_ = false;  // a rebuild is waiting for a mouse button to come up
	bool enginePending_ = false; // the ini changed during a stream; restart when it ends
	std::string last_; // last state JSON, empty while offline
	std::map<std::string, std::deque<double>> history_;
	std::chrono::steady_clock::time_point lastSample_{};
	std::unique_ptr<EventStream> events_;
};

// Last error reported by the embedded proxy, empty if none.
std::string LastError();

} // namespace livedelay
