// LiveDelay for OBS: a native panel and OBS hotkeys for the LiveDelay proxy.

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QMainWindow>
#include <QMetaObject>

#include <string>

#include <windows.h>
#include <shellapi.h>

#include "client.hpp"
#include "dock.hpp"

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

void OnEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		// Start the proxy with OBS if it isn't running yet. It keeps running after
		// OBS closes so a delayed stream can finish sending what it still holds.
		if (livedelay::Request("GET", "/state").empty() && dock)
			dock->StartProxy();
	}
}

} // namespace

bool obs_module_load(void)
{
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
	obs_frontend_add_event_callback(OnEvent, nullptr);
	blog(LOG_INFO, "[livedelay] loaded");
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(OnSave, nullptr);
	obs_frontend_remove_event_callback(OnEvent, nullptr);
	for (Hotkey &h : hotkeys)
		obs_hotkey_unregister(h.id);
	// OBS owns and deletes the dock widget.
	dock = nullptr;
}

const char *obs_module_name(void)
{
	return "LiveDelay";
}

const char *obs_module_description(void)
{
	return "Panel i skróty do sterowania opóźnieniem streamu LiveDelay.";
}
