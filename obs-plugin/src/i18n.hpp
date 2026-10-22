#pragma once

#include <obs-module.h>

#include <QString>

#include <functional>

namespace livedelay {

// Text from data/locale/<lang>.ini, then strings filled in from the server, then the key itself.
QString T(const char *key);

// When the installed locale file is behind the DLL, download the missing strings from
// livedelay.vxh.pl and reload them. `refreshed` runs on the UI thread if any text changed.
void StartLocaleHeal(std::function<void()> refreshed);

} // namespace livedelay
