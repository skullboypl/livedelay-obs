#include "settings.hpp"
#include "dock.hpp"
#include "i18n.hpp"
#include "services.hpp"
#include "client.hpp"

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
#include <QJsonDocument>
#include <QJsonObject>
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
			else if (k == "service")
				dest->service = v;
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
		else if (k == "language")
			s.language = v.empty() ? "auto" : v;
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
	for (const Destination &d : dests)
		o << "\n[dest " << d.name << "]\nservice = " << d.service << "\nurl = " << d.url << "\nkey = " << d.key
		  << "\nenabled = " << (d.enabled ? "yes" : "no") << "\nautostart = " << (d.autostart ? "yes" : "no") << "\n";
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
		general->addRow(T("Settings.DefaultDelay"), delay);
		general->addRow(T("Settings.BufferLimit"), buffer);
		general->addRow(T("Settings.Language"), language_);

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
		auto *editorRow = new QHBoxLayout;
		editorRow->addWidget(Hint(T("Settings.ThemeEditorAbout")), 1);
		editorRow->addWidget(editor);
		ovForm->addRow(QString(), editorRow);
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
		connect(service_, &QComboBox::activated, this, [this](int i) {
			FillServers(i > 0 ? service_->itemText(i) : QString(), QString());
			if (i > 0 && server_->count() > 0)
				url_->setText(server_->itemData(0).toString());
			if (i > 0 && (name_->text().trimmed().isEmpty() || name_->text() == T("Settings.NewPlatform"))) {
				name_->setText(service_->itemText(i).section(" - ", 0, 0));
				if (current_ >= 0)
					list_->item(current_)->setText(name_->text());
			}
		});
		connect(server_, &QComboBox::activated, this,
			[this](int i) { url_->setText(server_->itemData(i).toString()); });
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
		connect(editor, &QPushButton::clicked, this, [] {
			const char *locale = obs_get_locale();
			QDesktopServices::openUrl(QUrl(Q(OverlayUrl().substr(0, OverlayUrl().size() - 8) + "/edit-theme?lang=" +
						      std::string(locale ? locale : "en").substr(0, 2))));
		});
		connect(support, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kSupportUrl)); });
		connect(wizard, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl(kSmoothWizardUrl)); });
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [=] {
			Store();
			s_.delay = delay->value();
			s_.max_buffer_mb = buffer->value();
			SaveOverlay();
			s_.language = S(language_->currentData().toString());
			SaveAndRestart();
		});

		list_->setCurrentRow(s_.dests.empty() ? -1 : 0);
		Select(list_->currentRow());
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
		if (next == overlay_)
			return;
		int status = 0;
		Request("POST", "/overlay/config", QJsonDocument(next).toJson(QJsonDocument::Compact).toStdString(), &status);
		if (status != 200)
			QMessageBox::warning(this, "LiveDelay", T("Settings.SaveFailed"));
	}

	// Lists the servers (regions) of `service`, selecting the one matching `url`.
	void FillServers(const QString &service, const QString &url)
	{
		server_->clear();
		const Service *sv = service.isEmpty() ? nullptr : FindService(service);
		server_->setEnabled(sv != nullptr);
		url_->setReadOnly(false);
		keyLink_->setVisible(sv && !sv->key_link.isEmpty());
		if (!sv) {
			server_->addItem(T("Settings.CustomServer"));
			return;
		}
		if (!sv->key_link.isEmpty())
			keyLink_->setText(QString("<a href=\"%1\">%2</a>").arg(sv->key_link, T("Settings.GetKey")));
		int match = -1;
		for (const Server &srv : sv->servers) {
			if (srv.url == url)
				match = server_->count();
			server_->addItem(srv.name, srv.url);
		}
		if (match < 0 && !url.isEmpty()) {
			server_->insertItem(0, T("Settings.CustomServer"), url);
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
		d.key = S(key_->text());
		d.service = service_->currentIndex() > 0 ? S(service_->currentText()) : "";
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
		QString svc = Q(d.service);
		if (svc.isEmpty())
			if (const Service *found = ServiceForUrl(Q(d.url)))
				svc = found->name;
		int idx = svc.isEmpty() ? 0 : service_->findText(svc);
		service_->setCurrentIndex(idx < 0 ? 0 : idx);
		FillServers(idx > 0 ? svc : QString(), Q(d.url));
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
			service_->setFocus();
			return;
		}
		PointObsAtLiveDelay(s_.RtmpPort());
		QMessageBox::information(this, "LiveDelay", already ? T("Settings.AlreadyRouted") : T("Settings.Routed"));
	}

	void SaveAndRestart()
	{
		// Only a change the proxy cares about needs a restart; the language doesn't.
		Settings cmp = original_;
		cmp.language = s_.language;
		const bool proxyChanged = cmp.Text() != s_.Text();
		const bool languageChanged = original_.language != s_.language;
		if (proxyChanged && obs_frontend_streaming_active() &&
		    QMessageBox::question(this, "LiveDelay", T("Settings.RestartWarning")) != QMessageBox::Yes)
			return;
		if (!s_.Save(ConfigPath())) {
			QMessageBox::warning(this, "LiveDelay", T("Settings.SaveFailed"));
			return;
		}
		if (proxyChanged) {
			std::string err = StartProxy();
			if (!err.empty())
				QMessageBox::warning(this, "LiveDelay", Q(err));
		}
		if (languageChanged)
			ApplyLanguage(s_.language);
		accept();
	}

	Settings s_;
	Settings original_;
	QJsonObject overlay_;
	OverlayRow rows_[4];
	QComboBox *language_;
	int current_ = -1;
	QListWidget *list_;
	QWidget *fields_;
	QLineEdit *name_, *url_, *key_;
	QComboBox *service_;
	QComboBox *server_;
	QLabel *keyLink_;
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
