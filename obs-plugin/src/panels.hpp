#pragma once

#include <QString>
#include <QVector>

namespace livedelay {

// One browser dock LiveDelay can add to OBS for a platform.
struct BrowserPanel {
	const char *id; // stable; remembers which boxes were ticked
	const char *nameKey; // locale key for the dock's title
	const char *url; // {channel} is filled in, when there is one to fill
	bool needsChannel;
	bool onByDefault; // the everyday three are ticked; the rest are there if wanted
};

// What we have for a platform, empty when we have nothing for it. `service` is
// the name from OBS's service list, as a destination stores it.
QVector<BrowserPanel> PanelsForService(const QString &service);

// The dock's address, or empty when the panel wants a channel and has none.
QString PanelAddress(const BrowserPanel &panel, const QString &channel);

// The dock's title as the user will see it in OBS's Docks menu.
QString PanelTitle(const BrowserPanel &panel, const QString &destName);

// Remembers a dock to add. OBS rewrites its own list of browser docks while it
// closes, so nothing written during the session survives; the queue is merged
// into the config afterwards, on OBS_FRONTEND_EVENT_EXIT.
void QueueBrowserDock(const QString &title, const QString &url);
// Number of docks queued in this session, for telling the user what will appear.
int QueuedBrowserDockCount();
// Hooks the exit event. Call once from obs_module_load.
void InstallBrowserDockWriter();
void RemoveBrowserDockWriter();

} // namespace livedelay
