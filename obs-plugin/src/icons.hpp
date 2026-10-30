#pragma once

#include <QColor>
#include <QIcon>

namespace livedelay {

// Small line icons drawn in code, so the plugin ships no image files and they
// stay sharp at any scale.
enum class Icon { Arm, Activate, Live, LiveAfter, Disarm, Settings, Preview, Panel, Help, Restart, Play, Stop };

QIcon MakeIcon(Icon kind, const QColor &color);

} // namespace livedelay
