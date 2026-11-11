#pragma once

class QWidget;

namespace livedelay {

/// The vertical-stream wizard: finds Aitum Vertical, makes an input that
/// program publishes to, hands over the address and key, waits until the
/// picture really arrives, then points platforms at it. It writes the ini and
/// restarts the proxy itself, so it is opened with no settings window open.
void ShowVerticalWizard(QWidget *parent);

/// True when Aitum Vertical is loaded in this OBS. Asking OBS beats looking for
/// folders: it is what OBS actually loaded, wherever it came from.
bool AitumVerticalLoaded();

} // namespace livedelay
