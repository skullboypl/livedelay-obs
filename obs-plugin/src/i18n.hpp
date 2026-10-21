#pragma once

#include <obs-module.h>

#include <QString>

namespace livedelay {

// Text from data/locale/<lang>.ini, falling back to en-US.
inline QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace livedelay
