#pragma once

#include <cstddef>

namespace livedelay {

// Per-platform OBS encoders → LiveDelay ingest keys. Call Init once from the module.
void OutputsInit();
void OutputsShutdown();

// Start/stop all outputs that should follow OBS Start Streaming (autostart dests).
void OutputsOnStreamingStarting();
void OutputsOnStreamingStopping();

// Start/stop one destination's encoder+output (panel Start/Stop).
bool OutputStartDest(size_t index);
void OutputStopDest(size_t index);

// Rebuild encoder settings from ini (after settings save). Safe while stopped.
void OutputsReloadConfig();

} // namespace livedelay
