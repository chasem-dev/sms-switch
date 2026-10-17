// The PC's line into a test run (tools/switch-dev.py run). The Switch listens
// and the PC connects, so nothing on the PC has to accept a connection (a
// firewall there may refuse one, as it does nxlink -s's).
//
// With SMS_DEV_LISTEN=<port> among the launch arguments, the program waits up
// to a minute for the PC on that port, before anything reads its files:
//   1. the first connection brings files: "SMSF", then per file a u16 name
//      length, the name (a path under the NRO's folder), a u64 size and the
//      bytes; a zero length ends it. The program answers "OK\n".
//   2. a second connection, if the PC opens one at once, takes the window
//      captures (SMS_WINDOW_SHOTS): per capture "SHOT", u32 frame, width and
//      height, then the RGBA rows, bottom row first.
//   3. the first then carries stdout and stderr, as nxlink -s does.
// All little-endian.
#include "dev_link.h"
#include <switch.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>

extern "C" int __system_argc;
extern "C" char** __system_argv;

namespace {
int g_shot_socket = -1;
std::mutex g_shot_mutex;

bool recv_all(int s, void* buf, size_t size)
{
	for (char* p = (char*)buf; size;) {
		const ssize_t n = recv(s, p, size, 0);
		if (n <= 0)
			return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}

bool send_all(int s, const void* buf, size_t size)
{
	for (const char* p = (const char*)buf; size;) {
		const ssize_t n = send(s, p, size, 0);
		if (n <= 0)
			return false;
		p += n;
		size -= (size_t)n;
	}
	return true;
}

int accept_within(int listener, int ms)
{
	pollfd p = {listener, POLLIN, 0};
	if (poll(&p, 1, ms) <= 0)
		return -1;
	return accept(listener, NULL, NULL);
}

// What the first connection brings, written under dir; a line per file for
// the log, which is not this connection yet.
bool take_files(int s, const std::string& dir, std::string& report)
{
	char magic[4];
	if (!recv_all(s, magic, 4) || memcmp(magic, "SMSF", 4) != 0)
		return false;
	static char chunk[1 << 16];
	for (;;) {
		uint16_t len = 0;
		char name[256];
		uint64_t size = 0;
		if (!recv_all(s, &len, 2))
			return false;
		if (len == 0)
			return true;
		if (len >= sizeof name || !recv_all(s, name, len) || !recv_all(s, &size, 8))
			return false;
		name[len] = 0;
		const std::string path = dir + "/" + name;
		for (size_t slash = path.find('/', dir.size() + 1); slash != std::string::npos;
		     slash = path.find('/', slash + 1))
			mkdir(path.substr(0, slash).c_str(), 0777);
		const bool safe = name[0] != '/' && !strstr(name, "..");
		FILE* f = safe ? fopen((path + ".part").c_str(), "wb") : NULL;
		bool ok = true;
		for (uint64_t left = size; left && ok;) {
			const size_t n = left < sizeof chunk ? (size_t)left : sizeof chunk;
			ok = recv_all(s, chunk, n) && (!f || fwrite(chunk, 1, n, f) == n);
			left -= n;
		}
		if (f) {
			ok = fclose(f) == 0 && ok;
			remove(path.c_str());
			ok = ok && rename((path + ".part").c_str(), path.c_str()) == 0;
		}
		char line[400];
		snprintf(line, sizeof line, "[switch] dev link: %s, %llu bytes%s\n", name, (unsigned long long)size,
		         ok && safe ? "" : " (not written)");
		report += line;
		if (!ok)
			return false;
	}
}
} // namespace

int sms_dev_link_start(const std::string& dir)
{
	int port = 0;
	for (int i = 1; __system_argv && i < __system_argc; i++)
		if (!strncmp(__system_argv[i], "SMS_DEV_LISTEN=", 15))
			port = atoi(__system_argv[i] + 15);
	if (!port)
		return -1;
	socketInitializeDefault(); // already up is fine
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (listener < 0 || bind(listener, (sockaddr*)&addr, sizeof addr) != 0 || listen(listener, 2) != 0) {
		if (listener >= 0)
			close(listener);
		return -1;
	}
	const int s = accept_within(listener, 60000);
	std::string report;
	const bool files = s >= 0 && take_files(s, dir, report);
	if (s >= 0 && files && send_all(s, "OK\n", 3))
		g_shot_socket = accept_within(listener, 5000);
	close(listener);
	if (s < 0)
		return -1;
	fflush(stdout);
	fflush(stderr);
	dup2(s, STDOUT_FILENO);
	dup2(s, STDERR_FILENO);
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	fputs(report.c_str(), stderr);
	if (!files)
		fprintf(stderr, "[switch] dev link: the PC's files did not arrive whole\n");
	fprintf(stderr, "[switch] dev link: log on port %d%s\n", port,
	        g_shot_socket >= 0 ? ", window captures on a second connection" : "");
	return s;
}

// sms_gx's window captures (SMS_WINDOW_SHOTS), to the PC instead of the SD
// card while it listens (gx_render.cpp).
extern "C" bool GXPC_WindowShotSink(uint32_t frame, const uint8_t* rgba, int w, int h)
{
	std::lock_guard<std::mutex> lock(g_shot_mutex);
	if (g_shot_socket < 0)
		return false;
	const uint32_t head[3] = {frame, (uint32_t)w, (uint32_t)h};
	if (!send_all(g_shot_socket, "SHOT", 4) || !send_all(g_shot_socket, head, sizeof head) ||
	    !send_all(g_shot_socket, rgba, (size_t)w * h * 4)) {
		close(g_shot_socket);
		g_shot_socket = -1;
		return false;
	}
	return true;
}

void sms_dev_link_end(int stdio_socket)
{
	fflush(stdout);
	fflush(stderr);
	{
		std::lock_guard<std::mutex> lock(g_shot_mutex);
		if (g_shot_socket >= 0) {
			shutdown(g_shot_socket, SHUT_RDWR);
			close(g_shot_socket);
			g_shot_socket = -1;
		}
	}
	if (stdio_socket < 0)
		return;
	shutdown(stdio_socket, SHUT_RDWR);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
	close(stdio_socket);
}
