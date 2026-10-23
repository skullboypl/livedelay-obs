#pragma once

namespace livedelay {

// Anonymous heartbeat so the site can count copies of the plugin open in OBS.
void StartPresence();
void StopPresence();

} // namespace livedelay
