#include "client.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace livedelay {

namespace {

struct WinsockInit {
	WinsockInit()
	{
		WSADATA d;
		WSAStartup(MAKEWORD(2, 2), &d);
	}
	~WinsockInit() { WSACleanup(); }
};

WinsockInit &Winsock()
{
	static WinsockInit init;
	return init;
}

SOCKET Connect(int timeout_ms)
{
	Winsock();
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET)
		return s;
	DWORD t = timeout_ms;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&t), sizeof(t));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&t), sizeof(t));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kApiPort);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

bool SendAll(SOCKET s, const std::string &data)
{
	size_t sent = 0;
	while (sent < data.size()) {
		int n = send(s, data.data() + sent, int(data.size() - sent), 0);
		if (n <= 0)
			return false;
		sent += size_t(n);
	}
	return true;
}

std::string RequestLine(const char *method, const std::string &path)
{
	return std::string(method) + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

} // namespace

std::string Request(const char *method, const std::string &path, int *status)
{
	if (status)
		*status = 0;
	SOCKET s = Connect(3000);
	if (s == INVALID_SOCKET)
		return {};
	std::string resp;
	if (SendAll(s, RequestLine(method, path))) {
		char buf[4096];
		int n;
		while ((n = recv(s, buf, sizeof(buf), 0)) > 0)
			resp.append(buf, size_t(n));
	}
	closesocket(s);
	size_t body = resp.find("\r\n\r\n");
	if (body == std::string::npos)
		return {};
	if (status && resp.size() > 12)
		*status = std::atoi(resp.c_str() + 9);
	return resp.substr(body + 4);
}

EventStream::EventStream(std::function<void(const std::string &)> on_state, std::function<void()> on_offline)
	: on_state_(std::move(on_state)),
	  on_offline_(std::move(on_offline)),
	  thread_([this] { Run(); })
{
}

EventStream::~EventStream()
{
	stop_ = true;
	uintptr_t s = sock_.exchange(~uintptr_t(0));
	if (s != ~uintptr_t(0))
		closesocket(SOCKET(s)); // unblocks recv()
	if (thread_.joinable())
		thread_.join();
}

void EventStream::Run()
{
	bool was_online = true;
	while (!stop_) {
		// The proxy sends a snapshot at least every 15 s, so 30 s of silence means it is gone.
		SOCKET s = Connect(30000);
		if (s != INVALID_SOCKET) {
			sock_ = uintptr_t(s);
			if (stop_) {
				// Destructor may have run between Connect and the store above.
				if (sock_.exchange(~uintptr_t(0)) != ~uintptr_t(0))
					closesocket(s);
				break;
			}
			if (SendAll(s, RequestLine("GET", "/events"))) {
				std::string buf;
				char chunk[4096];
				int n;
				while (!stop_ && (n = recv(s, chunk, sizeof(chunk), 0)) > 0) {
					buf.append(chunk, size_t(n));
					size_t end;
					while ((end = buf.find("\n\n")) != std::string::npos) {
						std::string event = buf.substr(0, end);
						buf.erase(0, end + 2);
						size_t data = event.rfind("data: ");
						if (data != std::string::npos) {
							was_online = true;
							on_state_(event.substr(data + 6));
						}
					}
				}
			}
			if (sock_.exchange(~uintptr_t(0)) != ~uintptr_t(0))
				closesocket(s);
		}
		if (stop_)
			break;
		if (was_online) {
			was_online = false;
			on_offline_();
		}
		for (int i = 0; i < 20 && !stop_; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

static size_t FindValue(const std::string &json, const char *key, size_t from)
{
	std::string needle = std::string("\"") + key + "\":";
	size_t p = json.find(needle, from);
	return p == std::string::npos ? p : p + needle.size();
}

std::string JsonString(const std::string &json, const char *key, size_t from)
{
	size_t p = FindValue(json, key, from);
	if (p == std::string::npos || p >= json.size() || json[p] != '"')
		return {};
	std::string out;
	for (size_t i = p + 1; i < json.size() && json[i] != '"'; i++) {
		if (json[i] == '\\' && i + 1 < json.size())
			i++;
		out += json[i];
	}
	return out;
}

double JsonNumber(const std::string &json, const char *key, double fallback, size_t from)
{
	size_t p = FindValue(json, key, from);
	if (p == std::string::npos || json.compare(p, 4, "null") == 0)
		return fallback;
	return std::strtod(json.c_str() + p, nullptr);
}

bool JsonBool(const std::string &json, const char *key, size_t from)
{
	size_t p = FindValue(json, key, from);
	return p != std::string::npos && json.compare(p, 4, "true") == 0;
}

} // namespace livedelay
