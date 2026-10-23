#include "settings.hpp"
#include "serverpick.hpp"
#include "hotkeys.hpp"
#include "update.hpp"
#include "version.hpp"
#include "dock.hpp"
#include "encode.hpp"
#include "i18n.hpp"
#include "services.hpp"
#include "limits.hpp"
#include "log.hpp"
#include "outputs.hpp"
#include "client.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

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
				Destination d;
				d.name = Trim(sec.substr(4));
				s.dests.push_back(d);
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
			else if (k == "server")
				dest->server_pick = v;
			else if (k == "service")
				dest->service = v;
			else if (k == "key")
				dest->key = v;
			else if (k == "enabled")
				dest->enabled = Yes(v);
			else if (k == "autostart")
				dest->autostart = Yes(v);
			else if (k == "ingest_key")
				dest->ingest_key = v;
			else if (k == "encode_mode")
				dest->encode_mode = ParseEncodeMode(v);
			else if (k == "video_kbps")
				dest->video_kbps = uint32_t(std::atoi(v.c_str()));
			else if (k == "audio_kbps")
				dest->audio_kbps = uint32_t(std::atoi(v.c_str()));
			else if (k == "keyint_sec")
				dest->keyint_sec = uint32_t(std::atoi(v.c_str()));
			else if (k == "rate_control")
				dest->rate_control = v;
			else if (k == "width")
				dest->width = uint32_t(std::atoi(v.c_str()));
			else if (k == "height")
				dest->height = uint32_t(std::atoi(v.c_str()));
			else if (k == "vod_audio")
				dest->vod_audio = Yes(v);
			else if (k == "enhanced_broadcasting" || k == "enhanced")
				// Ignored until GoLive: multi-video RTMP breaks Twitch (#1000).
				dest->enhanced_broadcasting = false;
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
		else if (k == "language")
			s.language = v.empty() ? "auto" : v;
		else if (k.rfind("arm_slot_", 0) == 0) {
			const int slot = std::atoi(k.c_str() + 9);
			int sec = std::atoi(v.c_str());
			if (slot >= 0 && slot < 6) {
				if (sec < 1)
					sec = 1;
				if (sec > 3600)
					sec = 3600;
				s.arm_slot[slot] = sec;
			}
		}
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
	for (size_t i = 0; i < s.dests.size(); ++i) {
		if (s.dests[i].ingest_key.empty())
			s.dests[i].ingest_key = "ld" + std::to_string(i);
	}
	return s;
}

bool Settings::Save(const std::string &path) const
{
	std::string text = Text();
	std::string tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out << text;
		if (!out)
			return false;
	}
	return os_rename(tmp.c_str(), path.c_str()) == 0;
}

std::string Settings::Text() const
{
	std::ostringstream o;
	o << "# LiveDelay settings (edited from OBS: Tools -> LiveDelay)\n\n"
	  << "listen = " << listen << "\napi = " << api << "\ndelay = " << delay << "\nmax_buffer_mb = " << max_buffer_mb << "\nlanguage = " << language
	  << "\n";
	for (int i = 0; i < 6; ++i)
		o << "arm_slot_" << i << " = " << arm_slot[i] << "\n";
	for (size_t i = 0; i < dests.size(); ++i) {
		const Destination &d = dests[i];
		const std::string ingest = d.ingest_key.empty() ? ("ld" + std::to_string(i)) : d.ingest_key;
		o << "\n[dest " << d.name << "]\nservice = " << d.service << "\nurl = " << d.url;
		if (!d.server_pick.empty())
			o << "\nserver = " << d.server_pick;
		o << "\nkey = " << d.key
		  << "\ningest_key = " << ingest << "\nencode_mode = " << EncodeModeKey(d.encode_mode)
		  << "\nenabled = " << (d.enabled ? "yes" : "no") << "\nautostart = " << (d.autostart ? "yes" : "no");
		if (d.encode_mode == EncodeMode::Manual) {
			o << "\nvideo_kbps = " << d.video_kbps << "\naudio_kbps = " << d.audio_kbps
			  << "\nkeyint_sec = " << d.keyint_sec << "\nrate_control = " << d.rate_control
			  << "\nwidth = " << d.width << "\nheight = " << d.height;
		}
		o << "\nvod_audio = " << (d.vod_audio ? "yes" : "no")
		  << "\nenhanced_broadcasting = " << (d.enhanced_broadcasting ? "yes" : "no") << "\n";
	}
	return o.str();
}

void (*OnLanguageChanged)() = nullptr;

void ApplyLanguage(const std::string &language)
{
	const char *obs = obs_get_locale();
	obs_module_set_locale(language == "auto" || language.empty() ? (obs ? obs : "en-US") : language.c_str());
	if (OnLanguageChanged)
		OnLanguageChanged();
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
	out.service = name;
	already_livedelay = out.url.find("127.0.0.1:" + std::to_string(port)) != std::string::npos;
	if (out.url.empty() || out.url == "auto") {
		const Service *svcInfo = FindService(Q(name));
		out.url = svcInfo ? S(svcInfo->servers.front().url) : "";
	}
	return !out.url.empty() && !out.key.empty();
}

// OBS Multitrack / Enhanced Broadcasting needs a Twitch GoLive config URL. Custom
// RTMP (LiveDelay) has none → "Brak adresu URL z konfiguracją…". Turn it off and
// recreate the output handler so MultitrackVideoOutput is destroyed.
void TryResetObsOutputs()
{
	QWidget *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main)
		return;
	if (QMetaObject::invokeMethod(main, "ResetOutputs", Qt::DirectConnection)) {
		blog(LOG_INFO, "[livedelay] ResetOutputs via QMetaObject");
		return;
	}
#ifdef _WIN32
	// MSVC x64: OBSBasic::ResetOutputs() — this in RCX, same as void (*)(void*).
	if (HMODULE mod = GetModuleHandleW(nullptr)) {
		using ResetFn = void (*)(void *);
		if (auto fn = reinterpret_cast<ResetFn>(GetProcAddress(mod, "?ResetOutputs@OBSBasic@@QEAAXXZ"))) {
			fn(main);
			blog(LOG_INFO, "[livedelay] ResetOutputs via GetProcAddress");
			return;
		}
	}
#endif
	blog(LOG_WARNING,
	     "[livedelay] could not ResetOutputs; restart OBS once if Enhanced Broadcasting was on");
}

bool DisableObsStockEnhancedBroadcasting()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return false;
	const bool was_on = config_get_bool(cfg, "Stream1", "EnableMultitrackVideo");
	if (!was_on && !config_has_user_value(cfg, "Stream1", "EnableMultitrackVideo"))
		return false;
	config_set_bool(cfg, "Stream1", "EnableMultitrackVideo", false);
	config_save_safe(cfg, "tmp", nullptr);
	TryResetObsOutputs();
	if (was_on)
		blog(LOG_INFO, "[livedelay] disabled OBS Stream1 EnableMultitrackVideo (stock Enhanced Broadcasting)");
	return was_on;
}

bool RoutedTo(int port)
{
	obs_service_t *svc = obs_frontend_get_streaming_service();
	const char *url = svc ? obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : nullptr;
	return url && std::string(url).find("127.0.0.1:" + std::to_string(port)) != std::string::npos;
}

// Returns true when stock Multitrack/EB was on and we turned it off.
bool PointObsAtLiveDelay(int port)
{
	obs_data_t *s = obs_data_create();
	std::string server = "rtmp://127.0.0.1:" + std::to_string(port) + "/live";
	obs_data_set_string(s, "server", server.c_str());
	// Stock OBS encode is discarded by the proxy; real platforms use ld0… via plugin outputs.
	obs_data_set_string(s, "key", "_idle");
	obs_data_set_bool(s, "use_auth", false);
	obs_service_t *svc = obs_service_create("rtmp_custom", "default_service", s, nullptr);
	obs_frontend_set_streaming_service(svc);
	obs_frontend_save_streaming_service();
	obs_service_release(svc);
	obs_data_release(s);
	// Stock Multitrack/EB cannot negotiate a config URL for Custom → LiveDelay.
	const bool cleared_eb = DisableObsStockEnhancedBroadcasting();
	OutputsEnsureCustomServerVodTrack();
	// Do not change OBS → Output bitrates here: mode "obs" reads them as the
	// template for each platform. Stock publish to _idle is discarded by the proxy.
	return cleared_eb;
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

QScrollArea *ScrollWrap(QWidget *inner)
{
	auto *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	scroll->setWidget(inner);
	return scroll;
}

class SettingsDialog : public QDialog {
public:
	explicit SettingsDialog(QWidget *parent) : QDialog(parent), s_(Settings::Load(ConfigPath()))
	{
		setWindowTitle(T("Settings.Title"));
		setMinimumSize(520, 420);
		resize(720, 560);
		if (QScreen *scr = QApplication::primaryScreen()) {
			const QRect ag = scr->availableGeometry();
			resize(qMin(780, int(ag.width() * 0.9)), qMin(640, int(ag.height() * 0.85)));
		}

		delay_ = new QSpinBox;
		delay_->setRange(1, 3600);
		delay_->setSuffix(" s");
		delay_->setValue(s_.delay);
		buffer_ = new QSpinBox;
		buffer_->setRange(16, 16384);
		buffer_->setSuffix(" MB");
		buffer_->setValue(s_.max_buffer_mb);
		buffer_->setToolTip(T("Settings.BufferTip"));
		bufferSuggest_ = new QPushButton;
		bufferSuggest_->setToolTip(T("Settings.BufferTip"));
		bufferHint_ = new QLabel;
		bufferHint_->setWordWrap(true);
		bufferHint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
		auto *bufferRow = new QWidget;
		auto *bufferLay = new QVBoxLayout(bufferRow);
		bufferLay->setContentsMargins(0, 0, 0, 0);
		bufferLay->setSpacing(4);
		auto *bufferTop = new QHBoxLayout;
		bufferTop->setContentsMargins(0, 0, 0, 0);
		bufferTop->addWidget(buffer_, 1);
		bufferTop->addWidget(bufferSuggest_);
		bufferLay->addLayout(bufferTop);
		bufferLay->addWidget(bufferHint_);
		language_ = new QComboBox;
		const char *obsLocale = obs_get_locale();
		language_->addItem(T("Settings.LanguageAuto").arg(obsLocale ? obsLocale : "en-US"), "auto");
		language_->addItem("English", "en-US");
		language_->addItem("Polski", "pl-PL");
		language_->addItem("Deutsch", "de-DE");
		language_->addItem(QString::fromUtf8("Русский"), "ru-RU");
		{
			int i = language_->findData(Q(s_.language));
			language_->setCurrentIndex(i < 0 ? 0 : i);
		}
		original_ = s_;
		auto *general = new QFormLayout;
		general->addRow(T("Settings.DefaultDelay"), delay_);
		general->addRow(T("Settings.BufferLimit"), bufferRow);
		general->addRow(T("Settings.Language"), language_);
		connect(delay_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { RefreshBufferHint(); });
		connect(buffer_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { RefreshBufferHint(); });
		connect(bufferSuggest_, &QPushButton::clicked, this, [this] {
			const int rec = RecommendedBufferMb();
			if (rec > 0)
				buffer_->setValue(rec);
		});

		// Platforms: list on the left, the selected one's fields on the right.
		list_ = new QListWidget;
		list_->setMaximumWidth(170);
		name_ = new QLineEdit;
		service_ = new QComboBox;
		service_->addItem(T("Settings.CustomService"));
		for (const Service &sv : Services())
			service_->addItem(sv.name);
		server_ = new QComboBox;
		keyLink_ = new QLabel;
		keyLink_->setOpenExternalLinks(true);
		url_ = new QLineEdit;
		url_->setPlaceholderText(T("Settings.ServerPlaceholder"));
		key_ = new QLineEdit;
		key_->setEchoMode(QLineEdit::Password);
		auto *show = new QCheckBox(T("Settings.Show"));
		enabled_ = new QCheckBox(T("Settings.SendHere"));
		autostart_ = new QCheckBox(T("Settings.Autostart"));
		autostart_->setToolTip(T("Settings.AutostartTip"));
		encodeMode_ = new QComboBox;
		encodeMode_->addItem(T("Encode.ModeAuto"), int(EncodeMode::Auto));
		encodeMode_->addItem(T("Encode.ModeRecommended"), int(EncodeMode::Recommended));
		encodeMode_->addItem(T("Encode.ModeTwitchPartner"), int(EncodeMode::TwitchPartner));
		encodeMode_->addItem(T("Encode.ModeObs"), int(EncodeMode::Obs));
		encodeMode_->addItem(T("Encode.ModeManual"), int(EncodeMode::Manual));
		encodeMode_->setToolTip(T("Encode.ModeTip"));
		encodeSummary_ = new QLabel;
		encodeSummary_->setWordWrap(true);
		encodeSummary_->setStyleSheet("color: palette(placeholder-text);");
		videoKbps_ = new QSpinBox;
		videoKbps_->setRange(100, 100000);
		videoKbps_->setSuffix(" kb/s");
		audioKbps_ = new QSpinBox;
		audioKbps_->setRange(32, 512);
		audioKbps_->setSuffix(" kb/s");
		keyint_ = new QSpinBox;
		keyint_->setRange(0, 10);
		keyint_->setSuffix(" s");
		rateControl_ = new QComboBox;
		rateControl_->addItems({"CBR", "VBR", "CQP", "CRF"});
		width_ = new QSpinBox;
		width_->setRange(0, 7680);
		width_->setSpecialValueText(T("Encode.Canvas"));
		height_ = new QSpinBox;
		height_->setRange(0, 4320);
		height_->setSpecialValueText(T("Encode.Canvas"));
		vodAudio_ = new QCheckBox(T("Encode.VodAudio"));
		vodAudio_->setToolTip(T("Encode.VodAudioTip"));
		vodHelp_ = Hint(T("Encode.VodAudioHelp"));
		vodHelp_->hide();
		enhanced_ = new QCheckBox(T("Encode.Enhanced"));
		enhanced_->setToolTip(T("Encode.EnhancedTip"));
		auto *allObs = new QPushButton(T("Encode.AllObs"));
		allObs->setToolTip(T("Encode.AllObsTip"));
		auto *keyRow = new QHBoxLayout;
		keyRow->addWidget(key_, 1);
		keyRow->addWidget(show);
		auto *form = new QFormLayout;
		form->addRow(T("Settings.Name"), name_);
		form->addRow(T("Settings.Service"), service_);
		form->addRow(T("Settings.ServerRegion"), server_);
		form->addRow(T("Settings.Server"), url_);
		form->addRow(T("Settings.Key"), keyRow);
		form->addRow(QString(), keyLink_);
		enabled_->hide(); // superseded by autostart + Start/Stop in the panel
		form->addRow(QString(), autostart_);
		form->addRow(T("Encode.Mode"), encodeMode_);
		form->addRow(QString(), encodeSummary_);
		form->addRow(T("Encode.Video"), videoKbps_);
		form->addRow(T("Encode.Audio"), audioKbps_);
		form->addRow(T("Encode.Keyint"), keyint_);
		form->addRow(T("Encode.RateControl"), rateControl_);
		form->addRow(T("Encode.Width"), width_);
		form->addRow(T("Encode.Height"), height_);
		form->addRow(QString(), vodAudio_);
		form->addRow(QString(), vodHelp_);
		form->addRow(QString(), enhanced_);
		form->addRow(QString(), allObs);
		auto *fieldsInner = new QWidget;
		fieldsInner->setLayout(form);
		fields_ = ScrollWrap(fieldsInner);
		auto *add = new QPushButton(T("Settings.Add"));
		auto *remove = new QPushButton(T("Settings.Remove"));
		list_->setMinimumWidth(120);
		list_->setMaximumWidth(220);
		auto *listCol = new QVBoxLayout;
		listCol->addWidget(list_, 1);
		auto *listBtns = new QHBoxLayout;
		listBtns->addWidget(add);
		listBtns->addWidget(remove);
		listCol->addLayout(listBtns);
		auto *listWrap = new QWidget;
		listWrap->setLayout(listCol);
		auto *splitter = new QSplitter(Qt::Horizontal);
		splitter->addWidget(listWrap);
		splitter->addWidget(fields_);
		splitter->setStretchFactor(0, 0);
		splitter->setStretchFactor(1, 1);
		splitter->setSizes({160, 480});
		auto *importBtn = new QPushButton(T("Settings.SwitchObs"));
		auto *livePreview = new QPushButton(T("Settings.LivePreview"));
		livePreview->setToolTip(T("Settings.LivePreviewTip"));
		auto *helpBtn = new QPushButton(T("Dock.Help"));
		auto *destPage = new QWidget;
		auto *destLayout = new QVBoxLayout(destPage);
		destLayout->setContentsMargins(8, 8, 8, 8);
		destLayout->addWidget(Hint(T("Settings.PlatformsAbout")));
		destLayout->addWidget(splitter, 1);
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
		auto *editor = new QPushButton(T("Settings.ThemeEditor"));
		editor->setToolTip(T("Settings.ThemeEditorTip"));
		auto *urlRow = new QHBoxLayout;
		urlRow->addWidget(urlLine, 1);
		urlRow->addWidget(copy);
		urlRow->addWidget(preview);
		urlRow->addWidget(toScene);
		// Per state: show it or not, its text, and how it will look.
		overlay_ = QJsonDocument::fromJson(QByteArray::fromStdString(Request("GET", "/overlay/config"))).object();
		const QJsonObject texts = overlay_.value("texts").toObject(), showMap = overlay_.value("show").toObject(),
				  colors = overlay_.value("colors").toObject();
		auto *ovForm = new QFormLayout;
		ovForm->addRow(T("Settings.Address"), urlRow);
		const char *kinds[] = {"live", "arming", "armed", "delayed"};
		const char *labels[] = {"Settings.Live", "Settings.Arming", "Settings.Armed", "Settings.Delayed"};
		for (int i = 0; i < 4; i++) {
			OverlayRow &r = rows_[i];
			r.kind = kinds[i];
			r.color = colors.value(r.kind).toString("#3ddc84");
			r.show = new QCheckBox;
			r.show->setToolTip(T("Settings.ShowState"));
			r.show->setChecked(showMap.value(r.kind).toBool(true));
			r.text = new QLineEdit(texts.value(r.kind).toString());
			r.preview = new QLabel;
			r.preview->setMinimumWidth(150);
			auto *line = new QHBoxLayout;
			line->addWidget(r.show);
			line->addWidget(r.text, 1);
			line->addWidget(r.preview);
			ovForm->addRow(T(labels[i]), line);
			auto update = [this, i] { UpdateOverlayPreview(rows_[i]); };
			connect(r.show, &QCheckBox::toggled, this, update);
			connect(r.text, &QLineEdit::textChanged, this, update);
			UpdateOverlayPreview(r);
		}
		armedAsLive_ = new QCheckBox(T("Settings.ArmedAsLive"));
		armedAsLive_->setToolTip(T("Settings.ArmedAsLiveTip"));
		armedAsLive_->setChecked(overlay_.value("armedAsLive").toBool(false));
		ovForm->addRow(QString(), armedAsLive_);
		auto *editorRow = new QHBoxLayout;
		editorRow->addWidget(Hint(T("Settings.ThemeEditorAbout")), 1);
		editorRow->addWidget(editor);
		ovForm->addRow(QString(), editorRow);
		auto *ovPage = new QWidget;
		auto *ovLayout = new QVBoxLayout(ovPage);
		ovLayout->setContentsMargins(8, 8, 8, 8);
		ovLayout->addWidget(Hint(T("Settings.OverlayAbout")));
		ovLayout->addLayout(ovForm);
		ovLayout->addWidget(Hint(T("Settings.OverlayTips")));
		ovLayout->addStretch();

		auto *generalPage = new QWidget;
		auto *generalOuter = new QVBoxLayout(generalPage);
		generalOuter->setContentsMargins(8, 8, 8, 8);
		auto *generalTop = new QHBoxLayout;
		generalTop->addLayout(general, 1);
		generalTop->addWidget(helpBtn, 0, Qt::AlignTop);
		generalOuter->addLayout(generalTop);
		generalOuter->addStretch();

		auto *support = new QPushButton(T("Settings.SupportButton"));
		auto *wizard = new QPushButton(T("Settings.SmoothWizardButton"));
		auto *site = new QPushButton(T("Settings.WebsiteButton"));
		auto *checkUpdate = new QPushButton(T("Update.Check"));
		auto *downloadUpdate = new QPushButton(T("Update.UpdateNow"));
		downloadUpdate->hide();
		auto *updateStatus = new QLabel;
		updateStatus->setWordWrap(true);
		auto *versionLabel = new QLabel(T("Settings.Version").arg(QStringLiteral(LD_VERSION_STRING)));
		versionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
		auto *aboutPage = new QWidget;
		auto *aboutLayout = new QVBoxLayout(aboutPage);
		aboutLayout->setContentsMargins(8, 8, 8, 8);
		aboutLayout->addWidget(versionLabel);
		aboutLayout->addSpacing(8);
		aboutLayout->addWidget(Hint(T("Update.HowItWorks")));
		auto *updateRow = new QHBoxLayout;
		updateRow->setSpacing(8);
		updateRow->addWidget(checkUpdate, 0, Qt::AlignLeft);
		updateRow->addWidget(downloadUpdate, 0, Qt::AlignLeft);
		updateRow->addStretch();
		aboutLayout->addLayout(updateRow);
		updateStatus->setStyleSheet("color: palette(window-text);");
		aboutLayout->addWidget(updateStatus);
		aboutLayout->addSpacing(12);
		auto *streamDeck = Hint(T("Settings.StreamDeckText"));
		streamDeck->setOpenExternalLinks(true);
		aboutLayout->addWidget(streamDeck);
		aboutLayout->addSpacing(12);
		aboutLayout->addWidget(Hint(T("Settings.WebsiteText")));
		aboutLayout->addWidget(site, 0, Qt::AlignLeft);
		aboutLayout->addSpacing(12);
		aboutLayout->addWidget(Hint(T("Settings.SupportText")));
		aboutLayout->addWidget(support, 0, Qt::AlignLeft);
		aboutLayout->addSpacing(12);
		aboutLayout->addWidget(Hint(T("Settings.SmoothWizardText")));
		aboutLayout->addWidget(wizard, 0, Qt::AlignLeft);
		aboutLayout->addStretch();
		auto *updateChecker = new UpdateChecker(this);
		connect(checkUpdate, &QPushButton::clicked, this, [updateChecker, updateStatus, checkUpdate, downloadUpdate] {
			checkUpdate->setEnabled(false);
			downloadUpdate->hide();
			updateStatus->setText(T("Update.Checking"));
			updateChecker->Check(true);
		});
		connect(updateChecker, &UpdateChecker::found, this, [this, updateStatus, checkUpdate, downloadUpdate](UpdateInfo info) {
			checkUpdate->setEnabled(true);
			updateStatus->setText(T("Update.Available").arg(info.version).arg(UpdateChecker::CurrentVersion()));
			downloadUpdate->show();
			downloadUpdate->disconnect();
			QObject::connect(downloadUpdate, &QPushButton::clicked, downloadUpdate, [this, info] {
				UpdateChecker::ApplyUpdate(info, this);
			});
		});
		connect(updateChecker, &UpdateChecker::upToDate, this, [updateStatus, checkUpdate, downloadUpdate] {
			checkUpdate->setEnabled(true);
			downloadUpdate->hide();
			updateStatus->setText(T("Update.UpToDate").arg(UpdateChecker::CurrentVersion()));
		});
		connect(updateChecker, &UpdateChecker::failed, this, [updateStatus, checkUpdate, downloadUpdate](const QString &reason) {
			checkUpdate->setEnabled(true);
			downloadUpdate->hide();
			updateStatus->setText(T("Update.Failed").arg(reason));
		});

		auto *hotkeyPage = MakeHotkeyPage(hotkeyKeys_, hotkeySpins_);
		auto *tabs = new QTabWidget;
		tabs->addTab(ScrollWrap(generalPage), T("Settings.TabGeneral"));
		tabs->addTab(ScrollWrap(hotkeyPage), T("Settings.TabHotkeys"));
		tabs->addTab(destPage, T("Settings.TabPlatforms"));
		tabs->addTab(ScrollWrap(ovPage), T("Settings.TabOverlay"));
		tabs->addTab(MakeLogPage(), T("Settings.TabLogs"));
		tabs->addTab(ScrollWrap(aboutPage), T("Settings.TabAbout"));

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
		buttons->button(QDialogButtonBox::Save)->setText(T("Settings.Save"));
		buttons->button(QDialogButtonBox::Cancel)->setText(T("Settings.Cancel"));

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(10, 10, 10, 10);
		layout->addWidget(tabs, 1);
		layout->addWidget(buttons);

		for (const Destination &d : s_.dests)
			list_->addItem(Q(d.name));
		connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { Select(row); });
		connect(show, &QCheckBox::toggled, this,
			[this](bool on) { key_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
		connect(service_, &QComboBox::activated, this, [this](int i) {
			FillServers(i > 0 ? service_->itemText(i) : QString(), QString(), QString());
			if (i > 0 && server_->count() > 0)
				url_->setText(server_->itemData(0).toString());
			if (i > 0 && (name_->text().trimmed().isEmpty() || name_->text() == T("Settings.NewPlatform"))) {
				name_->setText(service_->itemText(i).section(" - ", 0, 0));
				if (current_ >= 0)
					list_->item(current_)->setText(name_->text());
			}
			UpdateEncodeFields();
		});
		connect(server_, &QComboBox::activated, this, [this](int i) {
			url_->setText(server_->itemData(i).toString());
			UpdateEncodeFields();
		});
		connect(url_, &QLineEdit::textChanged, this, [this] { UpdateEncodeFields(); });
		connect(vodAudio_, &QCheckBox::toggled, this, [this] { UpdateEncodeFields(); });
		connect(enhanced_, &QCheckBox::toggled, this, [this] { UpdateEncodeFields(); });
		connect(add, &QPushButton::clicked, this, [this] {
			Destination d;
			d.name = S(T("Settings.NewPlatform"));
			d.ingest_key = "ld" + std::to_string(s_.dests.size());
			s_.dests.push_back(d);
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
		connect(encodeMode_, &QComboBox::currentIndexChanged, this, [this] { UpdateEncodeFields(); });
		connect(videoKbps_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { UpdateEncodeFields(); });
		connect(audioKbps_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { UpdateEncodeFields(); });
		connect(allObs, &QPushButton::clicked, this, [this] {
			Store();
			for (Destination &d : s_.dests)
				d.encode_mode = EncodeMode::Obs;
			Select(current_);
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
		connect(editor, &QPushButton::clicked, this, [] {
			const char *locale = obs_get_locale();
			QDesktopServices::openUrl(QUrl(Q(OverlayUrl().substr(0, OverlayUrl().size() - 8) + "/edit-theme?lang=" +
						      std::string(locale ? locale : "en").substr(0, 2))));
		});
		connect(support, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kSupportUrl)); });
		connect(wizard, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kSmoothWizardUrl)); });
		connect(site, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl("https://livedelay.vxh.pl/")); });
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [this] {
			Store();
			s_.delay = delay_->value();
			s_.max_buffer_mb = buffer_->value();
			SaveOverlay();
			s_.language = S(language_->currentData().toString());
			for (int i = 0; i < 6 && i < int(hotkeySpins_.size()); ++i)
				s_.arm_slot[i] = hotkeySpins_[i]->value();
			ApplyHotkeyPage(hotkeyKeys_, hotkeySpins_);
			SaveAndRestart();
		});

		list_->setCurrentRow(s_.dests.empty() ? -1 : 0);
		Select(list_->currentRow());
		RefreshBufferHint();
	}

private:
	struct OverlayRow {
		QString kind, color;
		QCheckBox *show = nullptr;
		QLineEdit *text = nullptr;
		QLabel *preview = nullptr;
	};

	// A rough stand-in for the overlay pill, with the sample values the preview page uses.
	void UpdateOverlayPreview(OverlayRow &r)
	{
		QString t = r.text->text();
		t.replace("{s}", "25").replace("{buf}", "12");
		const bool on = r.show->isChecked() && !t.trimmed().isEmpty();
		r.text->setEnabled(r.show->isChecked());
		r.preview->setTextFormat(Qt::RichText);
		r.preview->setText(on ? QString("<span style='color:%1'>&#9679;</span>&nbsp;%2").arg(r.color, t.toHtmlEscaped())
				      : T("Settings.StateHidden"));
		r.preview->setStyleSheet(on ? "background: rgba(20,22,29,.9); color: #f3f4f7; border-radius: 10px; padding: 3px 10px; font-weight: 600;"
					    : "color: palette(placeholder-text); padding: 3px 10px; font-style: italic;");
	}

	// Texts and on/off go straight to the running overlay; no restart needed.
	void SaveOverlay()
	{
		if (overlay_.isEmpty())
			return;
		QJsonObject texts = overlay_.value("texts").toObject(), show = overlay_.value("show").toObject();
		for (const OverlayRow &r : rows_) {
			texts[r.kind] = r.text->text().trimmed();
			show[r.kind] = r.show->isChecked();
		}
		QJsonObject next = overlay_;
		next["texts"] = texts;
		next["show"] = show;
		next["armedAsLive"] = armedAsLive_ && armedAsLive_->isChecked();
		if (next == overlay_)
			return;
		int status = 0;
		Request("POST", "/overlay/config", QJsonDocument(next).toJson(QJsonDocument::Compact).toStdString(), &status);
		if (status != 200)
			QMessageBox::warning(this, "LiveDelay", T("Settings.SaveFailed"));
		else
			overlay_ = next;
	}

	// Lists the servers (regions) of `service`. `pick` is the saved row
	// ("auto" or a server name). Auto shares its URL with the closest ingest,
	// so a URL match alone would snap the combo back to that ingest.
	void FillServers(const QString &service, const QString &url, const QString &pick)
	{
		server_->clear();
		const Service *sv = service.isEmpty() ? nullptr : FindService(service);
		server_->setEnabled(sv != nullptr);
		url_->setReadOnly(false);
		keyLink_->setVisible(sv && !sv->key_link.isEmpty());
		if (!sv) {
			server_->addItem(T("Settings.CustomServer"), url);
			server_->setItemData(0, QString("custom"), Qt::UserRole + 1);
			return;
		}
		if (!sv->key_link.isEmpty())
			keyLink_->setText(QString("<a href=\"%1\">%2</a>").arg(sv->key_link, T("Settings.GetKey")));
		std::vector<ServerRow> rows;
		const QString autoName = T("Settings.AutoServer");
		for (const Server &srv : sv->servers) {
			const bool isAuto = srv.name == autoName;
			const int i = server_->count();
			server_->addItem(srv.name, srv.url);
			server_->setItemData(i, isAuto ? QString("auto") : srv.name, Qt::UserRole + 1);
			rows.push_back({isAuto, S(srv.name), S(srv.url)});
		}
		int match = PickServerRow(rows, S(pick), S(url));
		if (match < 0 && !url.isEmpty()) {
			server_->insertItem(0, T("Settings.CustomServer"), url);
			server_->setItemData(0, QString("custom"), Qt::UserRole + 1);
			match = 0;
		}
		server_->setCurrentIndex(match < 0 ? 0 : match);
	}

	void Store()
	{
		if (current_ < 0 || current_ >= int(s_.dests.size()))
			return;
		Destination &d = s_.dests[current_];
		d.name = S(name_->text()).empty() ? "Platform" : S(name_->text());
		d.url = S(url_->text());
		d.server_pick = S(server_->currentData(Qt::UserRole + 1).toString());
		if (d.server_pick == "custom")
			d.server_pick.clear();
		d.key = S(key_->text());
		d.service = service_->currentIndex() > 0 ? S(service_->currentText()) : "";
		d.enabled = true;
		d.autostart = autostart_->isChecked();
		d.encode_mode = EncodeMode(encodeMode_->currentData().toInt());
		d.video_kbps = uint32_t(videoKbps_->value());
		d.audio_kbps = uint32_t(audioKbps_->value());
		d.keyint_sec = uint32_t(keyint_->value());
		d.rate_control = S(rateControl_->currentText());
		d.width = uint32_t(width_->value());
		d.height = uint32_t(height_->value());
		d.vod_audio = vodAudio_->isChecked();
		d.enhanced_broadcasting = enhanced_->isChecked();
		if (d.ingest_key.empty() && current_ >= 0)
			d.ingest_key = "ld" + std::to_string(current_);
	}

	void UpdateEncodeFields()
	{
		const bool manual = encodeMode_->currentData().toInt() == int(EncodeMode::Manual);
		for (QWidget *w : {static_cast<QWidget *>(videoKbps_), static_cast<QWidget *>(audioKbps_),
				   static_cast<QWidget *>(keyint_), static_cast<QWidget *>(rateControl_),
				   static_cast<QWidget *>(width_), static_cast<QWidget *>(height_)})
			w->setEnabled(manual);
		const bool twitch = service_->currentText().contains("Twitch", Qt::CaseInsensitive) ||
				    url_->text().contains("twitch", Qt::CaseInsensitive) ||
				    url_->text().contains("live-video.net", Qt::CaseInsensitive);
		vodAudio_->setEnabled(twitch);
		vodHelp_->setVisible(twitch);
		// Multi-video EB without GoLive breaks Twitch (#1000) — keep UI off until GoLive ships.
		enhanced_->setEnabled(false);
		enhanced_->setChecked(false);
		if (!twitch)
			vodAudio_->setChecked(false);
		if (current_ < 0 || current_ >= int(s_.dests.size())) {
			encodeSummary_->clear();
			return;
		}
		Destination tmp = s_.dests[current_];
		tmp.encode_mode = EncodeMode(encodeMode_->currentData().toInt());
		tmp.video_kbps = uint32_t(videoKbps_->value());
		tmp.audio_kbps = uint32_t(audioKbps_->value());
		tmp.keyint_sec = uint32_t(keyint_->value());
		tmp.rate_control = S(rateControl_->currentText());
		tmp.width = uint32_t(width_->value());
		tmp.height = uint32_t(height_->value());
		tmp.vod_audio = vodAudio_->isChecked();
		tmp.enhanced_broadcasting = enhanced_->isChecked();
		tmp.service = service_->currentIndex() > 0 ? S(service_->currentText()) : tmp.service;
		tmp.url = S(url_->text());
		encodeSummary_->setText(ResolveEncode(tmp).summary);
		RefreshBufferHint();
	}

	// RAM for delay seconds at total kb/s: MB ≈ kbps × s / 8000 (matches Settings.BufferTip).
	int EstimateNeededMb(int *out_peak_kbps, int *out_platforms) const
	{
		const int delay_s = delay_ ? delay_->value() : s_.delay;
		int peak = 0;
		int n = 0;
		for (size_t i = 0; i < s_.dests.size(); ++i) {
			Destination d = s_.dests[i];
			if (int(i) == current_ && encodeMode_ && videoKbps_ && audioKbps_) {
				d.encode_mode = EncodeMode(encodeMode_->currentData().toInt());
				d.video_kbps = uint32_t(videoKbps_->value());
				d.audio_kbps = uint32_t(audioKbps_->value());
				d.keyint_sec = uint32_t(keyint_->value());
				d.rate_control = S(rateControl_->currentText());
				d.width = uint32_t(width_->value());
				d.height = uint32_t(height_->value());
				d.vod_audio = vodAudio_->isChecked();
				d.enhanced_broadcasting = enhanced_->isChecked();
				d.service = service_->currentIndex() > 0 ? S(service_->currentText()) : d.service;
				d.url = S(url_->text());
			}
			const EncodeSettings e = ResolveEncode(d);
			peak = std::max(peak, e.video_kbps + e.audio_kbps);
			++n;
		}
		if (n == 0) {
			const ObsOutput obs = ReadObsOutput();
			peak = (obs.video_kbps > 0 ? obs.video_kbps : 6000) + (obs.audio_kbps > 0 ? obs.audio_kbps : 160);
			n = 1;
		}
		if (out_peak_kbps)
			*out_peak_kbps = peak;
		if (out_platforms)
			*out_platforms = n;
		// Equal split across platforms: total limit must cover n × one full delay buffer.
		return int(std::ceil(double(peak) * double(delay_s) / 8000.0 * double(n)));
	}

	int RecommendedBufferMb() const
	{
		const int need = EstimateNeededMb(nullptr, nullptr);
		// ~25% headroom, then snap up to a friendly step.
		int rec = int(std::ceil(need * 1.25));
		if (rec < 64)
			rec = 64;
		const int steps[] = {64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384};
		for (int s : steps) {
			if (s >= rec)
				return s;
		}
		return 16384;
	}

	void RefreshBufferHint()
	{
		if (!bufferHint_ || !buffer_ || !bufferSuggest_)
			return;
		int peak = 0, n = 0;
		const int need = EstimateNeededMb(&peak, &n);
		const int limit = buffer_->value();
		const int delay_s = delay_->value();
		const int rec = RecommendedBufferMb();
		bufferSuggest_->setText(T("Settings.BufferSuggest").arg(rec));
		QString line;
		if (n > 1)
			line = T("Settings.BufferEstimateMulti").arg(need).arg(n).arg(delay_s).arg(peak);
		else
			line = T("Settings.BufferEstimate").arg(need).arg(delay_s).arg(peak);
		QString status;
		QString color;
		if (limit < need) {
			status = T("Settings.BufferLow");
			color = "#c44";
		} else if (limit < int(std::ceil(need * 1.15))) {
			status = T("Settings.BufferTight");
			color = "#b26a00";
		} else {
			const double head = need > 0 ? double(limit) / double(need) : 99.0;
			status = T("Settings.BufferOk").arg(head, 0, 'f', 1);
			color = "#2a7";
		}
		bufferHint_->setText(line + "\n" + status);
		bufferHint_->setStyleSheet(QString("color: %1; font-size: 11px;").arg(color));
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
		QString svc = Q(d.service);
		if (svc.isEmpty())
			if (const Service *found = ServiceForUrl(Q(d.url)))
				svc = found->name;
		int idx = svc.isEmpty() ? 0 : service_->findText(svc);
		service_->setCurrentIndex(idx < 0 ? 0 : idx);
		FillServers(idx > 0 ? svc : QString(), Q(d.url), Q(d.server_pick));
		// Auto's saved address is a snapshot of whichever ingest was closest.
		// Show the current one, so the next save follows "Automatycznie".
		if (server_->currentData(Qt::UserRole + 1).toString() == "auto") {
			const QString resolved = server_->currentData().toString();
			if (!resolved.isEmpty())
				url_->setText(resolved);
		}
		key_->setText(Q(d.key));
		enabled_->setChecked(d.enabled);
		autostart_->setChecked(d.autostart && d.enabled);
		const int modeIdx = encodeMode_->findData(int(d.encode_mode));
		encodeMode_->blockSignals(true);
		encodeMode_->setCurrentIndex(modeIdx < 0 ? encodeMode_->findData(int(EncodeMode::Obs)) : modeIdx);
		encodeMode_->blockSignals(false);
		videoKbps_->setValue(d.video_kbps > 0 ? int(d.video_kbps) : 6000);
		audioKbps_->setValue(d.audio_kbps > 0 ? int(d.audio_kbps) : 160);
		keyint_->setValue(int(d.keyint_sec));
		const int rc = rateControl_->findText(Q(d.rate_control.empty() ? "CBR" : d.rate_control));
		rateControl_->setCurrentIndex(rc < 0 ? 0 : rc);
		width_->setValue(int(d.width));
		height_->setValue(int(d.height));
		vodAudio_->setChecked(d.vod_audio);
		enhanced_->setChecked(d.enhanced_broadcasting);
		UpdateEncodeFields();
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
			Destination neu;
			neu.name = S(T("Settings.NewPlatform"));
			neu.ingest_key = "ld" + std::to_string(s_.dests.size());
			s_.dests.push_back(neu);
			list_->addItem(T("Settings.NewPlatform"));
			list_->setCurrentRow(list_->count() - 1);
			QMessageBox::information(this, "LiveDelay", T("Settings.RoutedNoTarget"));
			service_->setFocus();
			return;
		}
		PointObsAtLiveDelay(s_.RtmpPort());
		QMessageBox::information(this, "LiveDelay", already ? T("Settings.AlreadyRouted") : T("Settings.Routed"));
	}

	// What parts of the running system a settings save must touch.
	struct SaveImpact {
		bool proxy = false;   // listen / api / dest URL·key·ingest → tear down RTMP
		bool outputs = false; // encode / VOD / EB / dest add-remove → rebuild encoders
		bool hot = false;     // delay / max_buffer_mb → POST /settings, no disconnect
	};

	static bool DestProxySame(const Destination &a, const Destination &b)
	{
		return a.name == b.name && a.url == b.url && a.key == b.key && a.ingest_key == b.ingest_key &&
		       a.enabled == b.enabled && a.autostart == b.autostart && a.service == b.service;
	}

	static bool DestEncodeSame(const Destination &a, const Destination &b)
	{
		return a.encode_mode == b.encode_mode && a.video_kbps == b.video_kbps && a.audio_kbps == b.audio_kbps &&
		       a.keyint_sec == b.keyint_sec && a.rate_control == b.rate_control && a.width == b.width &&
		       a.height == b.height && a.vod_audio == b.vod_audio &&
		       a.enhanced_broadcasting == b.enhanced_broadcasting;
	}

	SaveImpact ClassifySave() const
	{
		SaveImpact i;
		if (original_.listen != s_.listen || original_.api != s_.api)
			i.proxy = true;
		if (original_.delay != s_.delay || original_.max_buffer_mb != s_.max_buffer_mb)
			i.hot = true;
		if (original_.dests.size() != s_.dests.size()) {
			i.proxy = true;
			i.outputs = true;
		} else {
			for (size_t n = 0; n < s_.dests.size(); ++n) {
				if (!DestProxySame(original_.dests[n], s_.dests[n]))
					i.proxy = true;
				if (!DestEncodeSame(original_.dests[n], s_.dests[n]))
					i.outputs = true;
			}
		}
		return i;
	}

	void SaveAndRestart()
	{
		const SaveImpact impact = ClassifySave();
		const bool languageChanged = original_.language != s_.language;
		const bool disruptive = impact.proxy || impact.outputs;
		if (disruptive && obs_frontend_streaming_active() &&
		    QMessageBox::question(this, "LiveDelay", T("Settings.RestartWarning")) != QMessageBox::Yes)
			return;
		if (!s_.Save(ConfigPath())) {
			QMessageBox::warning(this, "LiveDelay", T("Settings.SaveFailed"));
			return;
		}
		if (impact.proxy) {
			std::string err = StartProxy();
			if (!err.empty())
				QMessageBox::warning(this, "LiveDelay", Q(err));
			// Fresh proxy has empty dest publishers — rebuild OBS outputs and restart live ones.
			OutputsReloadConfig();
		} else {
			if (impact.hot) {
				const std::string q = "settings?delay=" + std::to_string(s_.delay) +
						      "&max_buffer_mb=" + std::to_string(s_.max_buffer_mb);
				int status = 0;
				Request("POST", "/" + q, &status);
				if (status != 200)
					blog(LOG_WARNING, "[livedelay] hot settings apply failed (HTTP %d)", status);
			}
			if (impact.outputs)
				OutputsReloadConfig();
		}
		if (languageChanged)
			ApplyLanguage(s_.language);
		accept();
	}

	Settings s_;
	Settings original_;
	QJsonObject overlay_;
	OverlayRow rows_[4];
	QCheckBox *armedAsLive_ = nullptr;
	QComboBox *language_;
	std::vector<ComboButton *> hotkeyKeys_;
	std::vector<QSpinBox *> hotkeySpins_;
	QSpinBox *delay_ = nullptr;
	QSpinBox *buffer_ = nullptr;
	QLabel *bufferHint_ = nullptr;
	QPushButton *bufferSuggest_ = nullptr;
	int current_ = -1;
	QListWidget *list_;
	QWidget *fields_;
	QLineEdit *name_, *url_, *key_;
	QComboBox *service_;
	QComboBox *server_;
	QLabel *keyLink_;
	QCheckBox *enabled_;
	QCheckBox *autostart_;
	QComboBox *encodeMode_;
	QLabel *encodeSummary_;
	QSpinBox *videoKbps_, *audioKbps_, *keyint_, *width_, *height_;
	QComboBox *rateControl_;
	QCheckBox *vodAudio_;
	QLabel *vodHelp_ = nullptr;
	QCheckBox *enhanced_;
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

void HardenObsForLiveDelay()
{
	const int port = Settings::Load(ConfigPath()).RtmpPort();
	if (!RoutedTo(port))
		return;
	DisableObsStockEnhancedBroadcasting();
	OutputsEnsureCustomServerVodTrack();
	// Stock key must be _idle. Legacy "livedelay" claimed slot ld0 → publisher busy
	// and Twitch got the stock encode (player error #1000).
	obs_service_t *svc = obs_frontend_get_streaming_service();
	const char *key = svc ? obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_STREAM_KEY) : nullptr;
	const bool bad_key = !key || !*key || strcmp(key, "_idle") != 0;
	if (bad_key) {
		blog(LOG_WARNING, "[livedelay] stock stream key was \"%s\" — forcing _idle", key ? key : "");
		PointObsAtLiveDelay(port);
	}
}

void RouteObsThroughLiveDelay(QWidget *parent)
{
	Settings s = Settings::Load(ConfigPath());
	Destination d;
	bool already = false;
	if (!CurrentObsTarget(d, already, s.RtmpPort()) && !already) {
		// Nothing to move (OBS had no server or key): route OBS and open the platform list.
		const bool cleared_eb = PointObsAtLiveDelay(s.RtmpPort());
		QString msg = T("Settings.RoutedNoTarget");
		if (cleared_eb)
			msg += "\n\n" + T("Settings.EbCleared");
		QMessageBox::information(parent, "LiveDelay", msg);
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
	const bool cleared_eb = PointObsAtLiveDelay(s.RtmpPort());
	OutputsReloadConfig();
	const bool fitted = !EncodeProblems().isEmpty() && FitEncodeForObsMode();
	QString msg = already ? T("Settings.AlreadyRouted") : T("Settings.RoutedSaved");
	if (cleared_eb)
		msg += "\n\n" + T("Settings.EbCleared");
	if (fitted)
		msg += "\n\n" + T("Limits.Fitted").arg(ReadObsOutput().video_kbps).arg(2);
	QMessageBox::information(parent, "LiveDelay", msg);
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
