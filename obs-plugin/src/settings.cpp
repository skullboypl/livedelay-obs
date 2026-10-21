#include "settings.hpp"
#include "dock.hpp"
#include "i18n.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdio>
#include <fstream>
#include <sstream>

extern "C" {
int ld_start(const char *config_path);
size_t ld_last_error(char *buf, size_t len);
}

namespace livedelay {

namespace {

std::string Trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	size_t b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool Yes(std::string v)
{
	for (char &c : v)
		c = char(tolower(static_cast<unsigned char>(c)));
	return v == "yes" || v == "tak" || v == "true" || v == "1" || v == "on";
}

QString Q(const std::string &s)
{
	return QString::fromStdString(s);
}

std::string S(const QString &s)
{
	return s.trimmed().toStdString();
}

struct Preset {
	const char *name;
	const char *url;
};

const Preset kPresets[] = {
	{"Twitch", "rtmp://live.twitch.tv/app"},
	{"YouTube", "rtmp://a.rtmp.youtube.com/live2"},
	{"Kick", "rtmps://fa723fc1b171.global-contribute.live-video.net/app"},
	{"Facebook", "rtmps://live-api-s.facebook.com:443/rtmp"},
};

} // namespace

Settings Settings::Load(const std::string &path)
{
	Settings s;
	std::ifstream in(path, std::ios::binary);
	std::string line;
	Destination *dest = nullptr;
	while (std::getline(in, line)) {
		line = Trim(line);
		if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF)
			line = line.substr(3); // BOM
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		if (line.front() == '[' && line.back() == ']') {
			std::string sec = Trim(line.substr(1, line.size() - 2));
			if (sec.rfind("dest", 0) == 0) {
				s.dests.push_back({Trim(sec.substr(4)), "", "", true});
				dest = &s.dests.back();
			}
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
		if (dest) {
			if (k == "url")
				dest->url = v;
			else if (k == "key")
				dest->key = v;
			else if (k == "enabled")
				dest->enabled = Yes(v);
			else if (k == "autostart")
				dest->autostart = Yes(v);
			continue;
		}
		if (k == "listen")
			s.listen = v;
		else if (k == "api")
			s.api = v;
		else if (k == "delay")
			s.delay = std::atoi(v.c_str());
		else if (k == "max_buffer_mb")
			s.max_buffer_mb = std::atoi(v.c_str());
		else if (k == "overlay_live")
			s.overlay_live = v;
		else if (k == "overlay_arming")
			s.overlay_arming = v;
		else if (k == "overlay_armed")
			s.overlay_armed = v;
		else if (k == "overlay_delayed")
			s.overlay_delayed = v;
	}
	if (s.delay < 1)
		s.delay = 15;
	if (s.max_buffer_mb < 16)
		s.max_buffer_mb = 512;
	return s;
}

bool Settings::Save(const std::string &path) const
{
	std::ostringstream o;
	o << "# LiveDelay settings (edited from OBS: Tools -> LiveDelay)\n\n"
	  << "listen = " << listen << "\napi = " << api << "\ndelay = " << delay << "\nmax_buffer_mb = " << max_buffer_mb
	  << "\n\n# Overlay: {s} = delay in seconds, {buf} = how much is buffered\n"
	  << "overlay_live = " << overlay_live << "\noverlay_arming = " << overlay_arming
	  << "\noverlay_armed = " << overlay_armed << "\noverlay_delayed = " << overlay_delayed << "\n";
	for (const Destination &d : dests)
		o << "\n[dest " << d.name << "]\nurl = " << d.url << "\nkey = " << d.key
		  << "\nenabled = " << (d.enabled ? "yes" : "no") << "\nautostart = " << (d.autostart ? "yes" : "no") << "\n";
	std::string tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out << o.str();
		if (!out)
			return false;
	}
	return os_rename(tmp.c_str(), path.c_str()) == 0;
}

int Settings::RtmpPort() const
{
	size_t c = listen.rfind(':');
	return c == std::string::npos ? 1935 : std::atoi(listen.c_str() + c + 1);
}

std::string ConfigPath()
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	char *p = obs_module_config_path("livedelay.ini");
	std::string path = p ? p : "livedelay.ini";
	bfree(p);
	return path;
}

std::string LastError()
{
	char buf[512];
	ld_last_error(buf, sizeof(buf));
	return buf;
}

std::string StartProxy()
{
	if (ld_start(ConfigPath().c_str()) == 0)
		return {};
	char buf[512];
	ld_last_error(buf, sizeof(buf));
	return buf;
}

static std::string ApiBase()
{
	Settings s = Settings::Load(ConfigPath());
	size_t c = s.api.rfind(':');
	return "http://127.0.0.1" + (c == std::string::npos ? std::string(":7799") : s.api.substr(c));
}

std::string OverlayUrl()
{
	return ApiBase() + "/overlay";
}

std::string LivePreviewUrl()
{
	const char *locale = obs_get_locale();
	return ApiBase() + "/live-preview?lang=" + std::string(locale ? locale : "en").substr(0, 2);
}

void AddOverlayToScene(QWidget *parent)
{
	obs_source_t *scene_src = obs_frontend_get_current_scene();
	obs_scene_t *scene = obs_scene_from_source(scene_src);
	if (!scene) {
		obs_source_release(scene_src);
		return;
	}
	obs_data_t *bs = obs_data_create();
	obs_data_set_string(bs, "url", OverlayUrl().c_str());
	obs_data_set_int(bs, "width", 900);
	obs_data_set_int(bs, "height", 140);
	obs_source_t *src = obs_source_create("browser_source", "LiveDelay overlay", bs, nullptr);
	if (src) {
		obs_scene_add(scene, src);
		obs_source_release(src);
		QMessageBox::information(parent, "LiveDelay",
					 T("Settings.OverlayAdded"));
	} else {
		QMessageBox::warning(parent, "LiveDelay",
				     T("Settings.OverlayFailed"));
	}
	obs_data_release(bs);
	obs_source_release(scene_src);
}

namespace {

// Reads the stream target OBS currently uses, so it can become a LiveDelay destination.
bool CurrentObsTarget(Destination &out, bool &already_livedelay, int port)
{
	obs_service_t *svc = obs_frontend_get_streaming_service();
	if (!svc)
		return false;
	const char *url = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	const char *key = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	obs_data_t *st = obs_service_get_settings(svc);
	std::string name = obs_data_get_string(st, "service");
	obs_data_release(st);
	out.url = url ? url : "";
	out.key = key ? key : "";
	out.name = name.empty() ? "Platform" : name;
	out.enabled = true;
	already_livedelay = out.url.find("127.0.0.1:" + std::to_string(port)) != std::string::npos;
	if (out.url.empty() || out.url == "auto") {
		for (const Preset &p : kPresets)
			if (name == p.name)
				out.url = p.url;
	}
	return !out.url.empty() && !out.key.empty();
}

void PointObsAtLiveDelay(int port)
{
	obs_data_t *s = obs_data_create();
	std::string server = "rtmp://127.0.0.1:" + std::to_string(port) + "/live";
	obs_data_set_string(s, "server", server.c_str());
	obs_data_set_string(s, "key", "livedelay");
	obs_service_t *svc = obs_service_create("rtmp_custom", "default_service", s, nullptr);
	obs_frontend_set_streaming_service(svc);
	obs_frontend_save_streaming_service();
	obs_service_release(svc);
	obs_data_release(s);
}

bool RoutedTo(int port)
{
	obs_service_t *svc = obs_frontend_get_streaming_service();
	const char *url = svc ? obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : nullptr;
	return url && std::string(url).find("127.0.0.1:" + std::to_string(port)) != std::string::npos;
}

// Links shown in the settings window only, never on stream.
constexpr const char *kSupportUrl = "https://donate.skullmedia.pl";
constexpr const char *kSmoothWizardUrl = "https://smoothwizard.com/?utm_source=livedelay-obs";

QLabel *Hint(const QString &text)
{
	auto *l = new QLabel(text);
	l->setWordWrap(true);
	l->setTextFormat(Qt::RichText);
	l->setStyleSheet("color: palette(placeholder-text);");
	return l;
}

class SettingsDialog : public QDialog {
public:
	explicit SettingsDialog(QWidget *parent) : QDialog(parent), s_(Settings::Load(ConfigPath()))
	{
		setWindowTitle(T("Settings.Title"));
		setMinimumWidth(660);

		auto *delay = new QSpinBox;
		delay->setRange(1, 3600);
		delay->setSuffix(" s");
		delay->setValue(s_.delay);
		auto *buffer = new QSpinBox;
		buffer->setRange(16, 16384);
		buffer->setSuffix(" MB");
		buffer->setValue(s_.max_buffer_mb);
		buffer->setToolTip(T("Settings.BufferTip"));
		auto *general = new QFormLayout;
		general->addRow(T("Settings.DefaultDelay"), delay);
		general->addRow(T("Settings.BufferLimit"), buffer);

		// Platforms: list on the left, the selected one's fields on the right.
		list_ = new QListWidget;
		list_->setMaximumWidth(170);
		name_ = new QLineEdit;
		preset_ = new QComboBox;
		preset_->addItem(T("Settings.ChoosePlatform"));
		for (const Preset &p : kPresets)
			preset_->addItem(p.name, QString(p.url));
		url_ = new QLineEdit;
		url_->setPlaceholderText(T("Settings.ServerPlaceholder"));
		key_ = new QLineEdit;
		key_->setEchoMode(QLineEdit::Password);
		auto *show = new QCheckBox(T("Settings.Show"));
		enabled_ = new QCheckBox(T("Settings.SendHere"));
		autostart_ = new QCheckBox(T("Settings.Autostart"));
		autostart_->setToolTip(T("Settings.AutostartTip"));
		auto *keyRow = new QHBoxLayout;
		keyRow->addWidget(key_, 1);
		keyRow->addWidget(show);
		auto *form = new QFormLayout;
		form->addRow(T("Settings.Name"), name_);
		form->addRow(T("Settings.Template"), preset_);
		form->addRow(T("Settings.Server"), url_);
		form->addRow(T("Settings.Key"), keyRow);
		form->addRow(QString(), enabled_);
		form->addRow(QString(), autostart_);
		fields_ = new QWidget;
		fields_->setLayout(form);
		auto *add = new QPushButton(T("Settings.Add"));
		auto *remove = new QPushButton(T("Settings.Remove"));
		auto *listCol = new QVBoxLayout;
		listCol->addWidget(list_, 1);
		auto *listBtns = new QHBoxLayout;
		listBtns->addWidget(add);
		listBtns->addWidget(remove);
		listCol->addLayout(listBtns);
		auto *destRow = new QHBoxLayout;
		destRow->addLayout(listCol);
		destRow->addWidget(fields_, 1);
		auto *importBtn = new QPushButton(T("Settings.SwitchObs"));
		auto *livePreview = new QPushButton(T("Settings.LivePreview"));
		livePreview->setToolTip(T("Settings.LivePreviewTip"));
		auto *helpBtn = new QPushButton(T("Dock.Help"));
		auto *destBox = new QGroupBox(T("Settings.Platforms"));
		auto *destLayout = new QVBoxLayout(destBox);
		destLayout->addWidget(Hint(T("Settings.PlatformsAbout")));
		destLayout->addLayout(destRow);
		auto *destBtns = new QHBoxLayout;
		destBtns->addWidget(importBtn, 1);
		destBtns->addWidget(livePreview);
		destLayout->addLayout(destBtns);

		// Overlay for a Browser source.
		auto *urlLine = new QLineEdit(Q(OverlayUrl()));
		urlLine->setReadOnly(true);
		auto *copy = new QPushButton(T("Settings.Copy"));
		auto *preview = new QPushButton(T("Settings.Preview"));
		preview->setToolTip(T("Settings.PreviewTip"));
		auto *toScene = new QPushButton(T("Settings.AddToScene"));
		auto *urlRow = new QHBoxLayout;
		urlRow->addWidget(urlLine, 1);
		urlRow->addWidget(copy);
		urlRow->addWidget(preview);
		urlRow->addWidget(toScene);
		auto *ovLive = new QLineEdit(Q(s_.overlay_live));
		auto *ovArming = new QLineEdit(Q(s_.overlay_arming));
		auto *ovArmed = new QLineEdit(Q(s_.overlay_armed));
		auto *ovDelayed = new QLineEdit(Q(s_.overlay_delayed));
		auto *ovForm = new QFormLayout;
		ovForm->addRow(T("Settings.Address"), urlRow);
		ovForm->addRow(T("Settings.Live"), ovLive);
		ovForm->addRow(T("Settings.Arming"), ovArming);
		ovForm->addRow(T("Settings.Armed"), ovArmed);
		ovForm->addRow(T("Settings.Delayed"), ovDelayed);
		auto *ovBox = new QGroupBox(T("Settings.Overlay"));
		auto *ovLayout = new QVBoxLayout(ovBox);
		ovLayout->addWidget(Hint(T("Settings.OverlayAbout")));
		ovLayout->addLayout(ovForm);
		ovLayout->addWidget(Hint(T("Settings.OverlayTips")));

		// Support and the author's other app: a quiet footer, not part of any output.
		auto *support = new QPushButton(T("Settings.SupportButton"));
		auto *wizard = new QPushButton(T("Settings.SmoothWizardButton"));
		auto *footer = new QFrame;
		footer->setObjectName("livedelayFooter");
		footer->setStyleSheet("#livedelayFooter { border: 1px solid palette(mid); border-radius: 6px; }");
		auto *footerLayout = new QGridLayout(footer);
		footerLayout->addWidget(Hint(T("Settings.SupportText")), 0, 0);
		footerLayout->addWidget(support, 0, 1);
		footerLayout->addWidget(Hint(T("Settings.SmoothWizardText")), 1, 0);
		footerLayout->addWidget(wizard, 1, 1);
		footerLayout->setColumnStretch(0, 1);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
		buttons->button(QDialogButtonBox::Save)->setText(T("Settings.Save"));
		buttons->button(QDialogButtonBox::Cancel)->setText(T("Settings.Cancel"));

		auto *top = new QHBoxLayout;
		top->addLayout(general, 1);
		top->addWidget(helpBtn, 0, Qt::AlignTop);
		auto *layout = new QVBoxLayout(this);
		layout->addLayout(top);
		layout->addWidget(destBox);
		layout->addWidget(ovBox);
		layout->addWidget(footer);
		layout->addWidget(buttons);

		for (const Destination &d : s_.dests)
			list_->addItem(Q(d.name));
		connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { Select(row); });
		connect(show, &QCheckBox::toggled, this,
			[this](bool on) { key_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
		connect(preset_, &QComboBox::activated, this, [this](int i) {
			if (i <= 0)
				return;
			url_->setText(preset_->itemData(i).toString());
			if (name_->text().trimmed().isEmpty() || name_->text() == T("Settings.NewPlatform"))
				name_->setText(preset_->itemText(i));
			if (current_ >= 0)
				list_->item(current_)->setText(name_->text());
			preset_->setCurrentIndex(0);
		});
		connect(add, &QPushButton::clicked, this, [this] {
			s_.dests.push_back({S(T("Settings.NewPlatform")), "", "", true});
			list_->addItem(T("Settings.NewPlatform"));
			list_->setCurrentRow(list_->count() - 1);
			name_->setFocus();
		});
		connect(remove, &QPushButton::clicked, this, [this] {
			int row = list_->currentRow();
			if (row < 0)
				return;
			current_ = -1;
			s_.dests.erase(s_.dests.begin() + row);
			delete list_->takeItem(row);
			Select(list_->currentRow());
		});
		connect(name_, &QLineEdit::textEdited, this, [this](const QString &t) {
			if (current_ >= 0)
				list_->item(current_)->setText(t);
		});
		connect(importBtn, &QPushButton::clicked, this, [this] { Import(); });
		connect(livePreview, &QPushButton::clicked, this,
			[] { QDesktopServices::openUrl(QUrl(Q(LivePreviewUrl()))); });
		connect(helpBtn, &QPushButton::clicked, this, [this] { ShowHelp(this); });
		connect(copy, &QPushButton::clicked, this,
			[urlLine] { QApplication::clipboard()->setText(urlLine->text()); });
		connect(preview, &QPushButton::clicked, this,
			[] { QDesktopServices::openUrl(QUrl(Q(OverlayUrl() + "?preview"))); });
		connect(toScene, &QPushButton::clicked, this, [this] { AddOverlayToScene(this); });
		connect(support, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kSupportUrl)); });
		connect(wizard, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kSmoothWizardUrl)); });
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [=] {
			Store();
			s_.delay = delay->value();
			s_.max_buffer_mb = buffer->value();
			s_.overlay_live = S(ovLive->text());
			s_.overlay_arming = S(ovArming->text());
			s_.overlay_armed = S(ovArmed->text());
			s_.overlay_delayed = S(ovDelayed->text());
			SaveAndRestart();
		});

		list_->setCurrentRow(s_.dests.empty() ? -1 : 0);
		Select(list_->currentRow());
	}

private:
	void Store()
	{
		if (current_ < 0 || current_ >= int(s_.dests.size()))
			return;
		Destination &d = s_.dests[current_];
		d.name = S(name_->text()).empty() ? "Platform" : S(name_->text());
		d.url = S(url_->text());
		d.key = S(key_->text());
		d.enabled = enabled_->isChecked();
		d.autostart = autostart_->isChecked();
	}

	void Select(int row)
	{
		Store();
		current_ = row;
		fields_->setEnabled(row >= 0);
		const Destination empty;
		const Destination &d = row >= 0 ? s_.dests[row] : empty;
		name_->setText(Q(d.name));
		url_->setText(Q(d.url));
		key_->setText(Q(d.key));
		enabled_->setChecked(d.enabled);
		autostart_->setChecked(d.autostart);
	}

	void Import()
	{
		Store();
		Destination d;
		bool already = false;
		if (CurrentObsTarget(d, already, s_.RtmpPort()) && !already) {
			s_.dests.push_back(d);
			list_->addItem(Q(d.name));
			list_->setCurrentRow(list_->count() - 1);
		} else if (!already) {
			// OBS had no server/key yet: route it anyway and let the user add the platform here.
			PointObsAtLiveDelay(s_.RtmpPort());
			s_.dests.push_back({S(T("Settings.NewPlatform")), "", "", true});
			list_->addItem(T("Settings.NewPlatform"));
			list_->setCurrentRow(list_->count() - 1);
			QMessageBox::information(this, "LiveDelay", T("Settings.RoutedNoTarget"));
			preset_->setFocus();
			return;
		}
		PointObsAtLiveDelay(s_.RtmpPort());
		QMessageBox::information(this, "LiveDelay", already ? T("Settings.AlreadyRouted") : T("Settings.Routed"));
	}

	void SaveAndRestart()
	{
		if (obs_frontend_streaming_active() &&
		    QMessageBox::question(this, "LiveDelay", T("Settings.RestartWarning")) != QMessageBox::Yes)
			return;
		if (!s_.Save(ConfigPath())) {
			QMessageBox::warning(this, "LiveDelay", T("Settings.SaveFailed"));
			return;
		}
		std::string err = StartProxy();
		if (!err.empty())
			QMessageBox::warning(this, "LiveDelay", Q(err));
		accept();
	}

	Settings s_;
	int current_ = -1;
	QListWidget *list_;
	QWidget *fields_;
	QLineEdit *name_, *url_, *key_;
	QComboBox *preset_;
	QCheckBox *enabled_;
	QCheckBox *autostart_;
};

} // namespace

void ShowSettings(QWidget *parent)
{
	SettingsDialog dlg(parent);
	dlg.exec();
}

bool ObsRoutedToLiveDelay()
{
	return RoutedTo(Settings::Load(ConfigPath()).RtmpPort());
}

void RouteObsThroughLiveDelay(QWidget *parent)
{
	Settings s = Settings::Load(ConfigPath());
	Destination d;
	bool already = false;
	if (!CurrentObsTarget(d, already, s.RtmpPort()) && !already) {
		// Nothing to move (OBS had no server or key): route OBS and open the platform list.
		PointObsAtLiveDelay(s.RtmpPort());
		QMessageBox::information(parent, "LiveDelay", T("Settings.RoutedNoTarget"));
		ShowSettings(parent);
		return;
	}
	if (!already) {
		bool known = false;
		for (const Destination &x : s.dests)
			known |= x.url == d.url && x.key == d.key;
		if (!known)
			s.dests.push_back(d);
		if (!s.Save(ConfigPath())) {
			QMessageBox::warning(parent, "LiveDelay", T("Settings.SaveFailed"));
			return;
		}
		StartProxy();
	}
	PointObsAtLiveDelay(s.RtmpPort());
	QMessageBox::information(parent, "LiveDelay", already ? T("Settings.AlreadyRouted") : T("Settings.RoutedSaved"));
}

void ShowHelp(QWidget *parent)
{
	QDialog dlg(parent);
	dlg.setWindowTitle(T("Help.Title"));
	dlg.setMinimumWidth(560);
	auto *body = new QLabel(T("Help.Body").arg(Q(OverlayUrl())));
	body->setWordWrap(true);
	body->setTextFormat(Qt::RichText);
	body->setTextInteractionFlags(Qt::TextBrowserInteraction);
	body->setOpenExternalLinks(true);
	auto *route = new QPushButton(T("Help.RouteNow"));
	route->setEnabled(!ObsRoutedToLiveDelay());
	auto *settings = new QPushButton(T("Dock.Settings"));
	auto *close = new QPushButton(T("Help.Close"));
	auto *buttons = new QHBoxLayout;
	buttons->addWidget(route);
	buttons->addWidget(settings);
	buttons->addStretch();
	buttons->addWidget(close);
	auto *layout = new QVBoxLayout(&dlg);
	layout->addWidget(body);
	layout->addLayout(buttons);
	QObject::connect(route, &QPushButton::clicked, &dlg, [&dlg, route] {
		RouteObsThroughLiveDelay(&dlg);
		route->setEnabled(!ObsRoutedToLiveDelay());
	});
	QObject::connect(settings, &QPushButton::clicked, &dlg, [&dlg] {
		dlg.accept();
		ShowSettings(dlg.parentWidget());
	});
	QObject::connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
	dlg.exec();
}

} // namespace livedelay
