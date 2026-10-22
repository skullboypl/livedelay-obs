#pragma once

class QWidget;

namespace livedelay {

// Keeps the recent [livedelay] lines OBS already writes, so the settings tab
// can show why a stream did not start without opening the OBS log file.
void InstallLogCapture();
void UninstallLogCapture();

QWidget *MakeLogPage();

} // namespace livedelay
