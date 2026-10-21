// Minimal HTTP client for the LiveDelay proxy on 127.0.0.1. Plain Winsock: the
// proxy speaks a tiny fixed dialect, so pulling in a network library isn't worth it.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace livedelay {

constexpr uint16_t kApiPort = 7799;

// Sends one request and returns the response body, or an empty string when the
// proxy is unreachable. `status` gets the HTTP status code (0 on failure).
std::string Request(const char *method, const std::string &path, int *status = nullptr);

// Follows GET /events on a background thread. `on_state` gets each JSON
// snapshot, `on_offline` fires when the connection drops; both run on that thread.
class EventStream {
public:
	EventStream(std::function<void(const std::string &)> on_state, std::function<void()> on_offline);
	~EventStream();

private:
	void Run();

	std::function<void(const std::string &)> on_state_;
	std::function<void()> on_offline_;
	std::atomic<bool> stop_{false};
	std::atomic<uintptr_t> sock_{~uintptr_t(0)};
	std::thread thread_;
};

// Tiny readers for the proxy's flat JSON. Return the fallback when the key is missing.
std::string JsonString(const std::string &json, const char *key, size_t from = 0);
double JsonNumber(const std::string &json, const char *key, double fallback = 0);
bool JsonBool(const std::string &json, const char *key);

} // namespace livedelay
