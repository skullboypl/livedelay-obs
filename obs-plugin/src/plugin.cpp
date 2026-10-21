// LiveDelay for OBS: the delay proxy runs inside OBS, with a native panel,
// OBS hotkeys, a settings window and a Browser-source overlay.

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QMainWindow>
#include <QMetaObject>

#include <string>

#include "dock.hpp"
#include "settings.hpp"

extern "C" {
typedef void (*ld_log_fn)(const char *msg);
void ld_set_logger(ld_log_fn f);
void ld_stop(void);
}

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("skullboypl")

namespace {

struct Hotkey {
	const char *name;
	const char *description;
	const char *action;
	obs_hotkey_id id;
};

Hotkey hotkeys[] = {
	{"livedelay.arm", "LiveDelay: Arm", "arm", OBS_INVALID_HOTKEY_ID},
	{"livedelay.activate", "LiveDelay: Activate (włącz opóźnienie)", "activate", OBS_INVALID_HOTKEY_ID},
	{"livedelay.cut", "LiveDelay: Cut (wróć na żywo)", "cut", OBS_INVALID_HOTKEY_ID},
	{"livedelay.cut_after", "LiveDelay: Cut po wyemitowaniu", "cut-after", OBS_INVALID_HOTKEY_ID},
};

livedelay::Dock *dock = nullptr;

void OnHotkey(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !dock)
		return;
	// Hotkeys fire on OBS's hotkey thread; the panel lives on the UI thread.
	std::string action = static_cast<Hotkey *>(data)->action;
	QMetaObject::invokeMethod(
		dock, [action] { if (dock) dock->Hotkey(action); }, Qt::QueuedConnection);
}

void OnSave(obs_data_t *data, bool saving, void *)
{
	for (Hotkey &h : hotkeys) {
		if (saving) {
			obs_data_array_t *a = obs_hotkey_save(h.id);
			obs_data_set_array(data, h.name, a);
			obs_data_array_release(a);
		} else {
			obs_data_array_t *a = obs_data_get_array(data, h.name);
			obs_hotkey_load(h.id, a);
			obs_data_array_release(a);
		}
	}
}

void LogFromProxy(const char *msg)
{
	blog(LOG_INFO, "[livedelay] %s", msg);
}

} // namespace

bool obs_module_load(void)
{
	ld_set_logger(LogFromProxy);
	std::string err = livedelay::StartProxy();
	if (!err.empty())
		blog(LOG_WARNING, "[livedelay] %s", err.c_str());

	for (Hotkey &h : hotkeys)
		h.id = obs_hotkey_register_frontend(h.name, h.description, OnHotkey, &h);

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
	for (Hotkey &h : hotkeys)
		obs_hotkey_unregister(h.id);
	// OBS owns and deletes the dock widget.
	dock = nullptr;
	ld_stop();
}

const char *obs_module_name(void)
{
	return "LiveDelay";
}

const char *obs_module_description(void)
{
	return "Przełączane opóźnienie streamu: panel, skróty, overlay.";
}
