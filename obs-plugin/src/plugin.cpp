// LiveDelay for OBS: the delay proxy runs inside OBS, with a native panel,
// OBS hotkeys, a settings window and a Browser-source overlay.

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QApplication>
#include <QMainWindow>
#include <QMetaObject>

#include <string>

#include "dock.hpp"
#include "hotkeys.hpp"
#include "outputs.hpp"
#include "settings.hpp"

extern "C" {
typedef void (*ld_log_fn)(const char *msg);
void ld_set_logger(ld_log_fn f);
void ld_stop(void);
}

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("skullboypl")
OBS_MODULE_USE_DEFAULT_LOCALE("livedelay-obs", "en-US")

namespace {

livedelay::Dock *dock = nullptr;

void OnSave(obs_data_t *data, bool saving, void *)
{
	if (saving)
		livedelay::SaveHotkeyData(data);
	else
		livedelay::LoadHotkeyData(data);
}

void LogFromProxy(const char *msg)
{
	blog(LOG_INFO, "[livedelay] %s", msg);
}

} // namespace

bool obs_module_load(void)
{
	ld_set_logger(LogFromProxy);
	// A language picked in LiveDelay's settings wins over OBS's.
	const std::string language = livedelay::Settings::Load(livedelay::ConfigPath()).language;
	if (language != "auto")
		livedelay::ApplyLanguage(language);
	livedelay::OnLanguageChanged = [] {
		if (dock)
			dock->Retranslate();
	};
	std::string err = livedelay::StartProxy();
	if (!err.empty())
		blog(LOG_WARNING, "[livedelay] %s", err.c_str());
	livedelay::OutputsInit();

	livedelay::RegisterHotkeys([](const std::string &action) {
		// Hotkeys fire on OBS's hotkey thread; the panel lives on the UI thread.
		QMetaObject::invokeMethod(
			qApp, [action] { if (dock) dock->Hotkey(action); }, Qt::QueuedConnection);
	});

	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dock = new livedelay::Dock(main);
	if (!obs_frontend_add_dock_by_id("livedelay", "LiveDelay", dock)) {
		delete dock;
		dock = nullptr;
		blog(LOG_WARNING, "[livedelay] could not add dock");
	}
	obs_frontend_add_save_callback(OnSave, nullptr);
	obs_frontend_add_tools_menu_item(
		"LiveDelay",
		[](void *) { livedelay::ShowSettings(static_cast<QWidget *>(obs_frontend_get_main_window())); },
		nullptr);
	blog(LOG_INFO, "[livedelay] loaded");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(OnSave, nullptr);
	livedelay::UnregisterHotkeys();
	// OBS owns and deletes the dock widget.
	dock = nullptr;
	livedelay::OutputsShutdown();
	ld_stop();
}

const char *obs_module_name(void)
{
	return "LiveDelay";
}

const char *obs_module_description(void)
{
	return obs_module_text("Plugin.Description");
}
