// Switch start-up and shut-down around the port's own main()
// (sms-port src/port_main.cpp).
//
// Files live beside the NRO (libnx makes its folder the working directory),
// normally sdmc:/switch/sms/ (sdmc:/switch/sms-eclipse/ for sms-eclipse.nro):
//   *.iso / *.gcm / *.ciso   the disc image (also in rom/), unless this is
//                            sms-standalone.nro, which carries it in its RomFS
//   settings.txt, bindings.txt
//   save/    memory card files      cache/   shader cache
//   sms.log  the port's log (stderr), unless started with nxlink
//   (tools/switch-dev.py run --file sends other files here as it starts)
#include <switch.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include "low_memory.h"
#ifdef SMS_SWITCH_DKGL
#include "dkgl/dkgl.h"
#endif

// The program's name and title (CMakeLists.txt): sms and Super Mario Sunshine,
// or sms-eclipse and Super Mario Eclipse.
#ifndef SMS_SWITCH_APP
#define SMS_SWITCH_APP "sms"
#endif
#ifndef SMS_SWITCH_TITLE
#define SMS_SWITCH_TITLE "Super Mario Sunshine"
#endif


// The button prompt images (CMakeLists.txt: switch/prompts/switch.png).
extern "C" const unsigned char sms_switch_prompts_png[];
extern "C" const unsigned long sms_switch_prompts_png_size;
extern "C" int __system_argc; // libnx's argv, before main
extern "C" char** __system_argv;
extern "C" void sms_switch_spread_main_thread(void); // performance.cpp
extern "C" void sms_switch_start_clocks(void);

// A line in the emulator's or debugger's log, before stderr is set up.
void sms_switch_trace(const char* text)
{
	svcOutputDebugString(text, strlen(text));
}

namespace {
// Plain storage: switch_start runs before other constructors.
char g_dir_buf[512] = "sdmc:/switch/" SMS_SWITCH_APP;
int g_nxlink_socket = -1;

bool is_disc_image(const char* name)
{
	const char* dot = strrchr(name, '.');
	return dot && (!strcasecmp(dot, ".iso") || !strcasecmp(dot, ".gcm") || !strcasecmp(dot, ".ciso"));
}

std::string find_disc_image()
{
	const std::string g_dir = g_dir_buf;
	for (const std::string& dir : {g_dir, g_dir + "/rom"}) {
		DIR* d = opendir(dir.c_str());
		if (!d)
			continue;
		std::string found;
		while (dirent* e = readdir(d)) {
			if (is_disc_image(e->d_name)) {
				found = dir + "/" + e->d_name;
				break;
			}
		}
		closedir(d);
		if (!found.empty())
			return found;
	}
	return std::string();
}

// env.txt beside the NRO: NAME=value lines become environment variables
// (the PC port's switches, docs/DEVELOPMENT.md in sms-port), as a shell
// would set them. # starts a comment.
void load_env_file(const std::string& path)
{
	FILE* f = fopen(path.c_str(), "r");
	if (!f)
		return;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		char* s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '#' || *s == '\n' || *s == '\r' || !*s)
			continue;
		char* eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = 0;
		char* value = eq + 1;
		for (size_t n = strlen(value); n && (value[n - 1] == '\n' || value[n - 1] == '\r');)
			value[--n] = 0;
		for (size_t n = strlen(s); n && (s[n - 1] == ' ' || s[n - 1] == '\t');)
			s[--n] = 0;
		setenv(s, value, 1);
		fprintf(stderr, "[switch] env.txt: %s=%s\n", s, value);
	}
	fclose(f);
}

// The image packed into sms-standalone.nro's RomFS (sms-eclipse-standalone.nro's) (romfs:/disc.iso, .gcm or
// .ciso; CMake's SMS_SWITCH_BUNDLE_DISC), if this is that build.
std::string find_bundled_disc_image()
{
	// hbmenu: the NRO's own file (argv[0]). An emulator starting the NRO
	// directly passes no path but serves it as the process's data storage
	// (libnx would take that path itself had low_memory.cpp not given the
	// moved program a homebrew context).
	if (R_FAILED(romfsInit())) {
		if (envHasArgv())
			return std::string(); // hbmenu started an NRO without a RomFS: sms.nro
		FsStorage storage;
		if (R_FAILED(fsOpenDataStorageByCurrentProcess(&storage)))
			return std::string(); // sms.nro: no RomFS
		if (R_FAILED(romfsMountFromStorage(storage, 0, "romfs"))) {
			fsStorageClose(&storage);
			return std::string();
		}
	}
	for (const char* name : {"romfs:/disc.iso", "romfs:/disc.gcm", "romfs:/disc.ciso"}) {
		struct stat st;
		if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
			fprintf(stderr, "[switch] disc image packed in this NRO (%s, %lld MiB)\n", name,
			        (long long)(st.st_size >> 20));
			return name;
		}
	}
	romfsExit();
	return std::string();
}

// Started from the Album (applet mode) instead of through a game (title
// takeover, hold R): far less memory, too little for the game and Mesa's
// GPU context (on a 16.0.2 console: 86 MiB left after the game's MEM1,
// and the window surface could not be made). Say so on screen and wait.
bool applet_mode()
{
	const AppletType type = appletGetAppletType();
	return type != AppletType_Application && type != AppletType_SystemApplication;
}

void explain_applet_mode()
{
	consoleInit(NULL);
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	PadState pad;
	padInitializeDefault(&pad);
	printf("\n\n  " SMS_SWITCH_TITLE " needs more memory than the Album gives homebrew.\n\n"
	       "  Start it through a game instead: hold R while you start any game,\n"
	       "  keep holding until the homebrew menu appears, then pick it there.\n\n"
	       "  (The log is sms.log beside the NRO.)\n\n"
	       "  Press + to close.\n");
	while (appletMainLoop()) {
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_A | HidNpadButton_B))
			break;
		consoleUpdate(NULL);
	}
	consoleExit(NULL);
}

// True if settings.txt in `dir` sets `name` (then it, not a Switch default, wins).
bool settings_has(const std::string& dir, const char* name)
{
	FILE* f = fopen((dir + "/settings.txt").c_str(), "r");
	if (!f)
		return false;
	char line[512];
	bool found = false;
	const size_t n = strlen(name);
	while (!found && fgets(line, sizeof line, f)) {
		const char* s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		found = strncmp(s, name, n) == 0 && (s[n] == ' ' || s[n] == '\t' || s[n] == '=');
	}
	fclose(f);
	return found;
}

// SMS_DEV_FETCH=<port> (tools/switch-dev.py run --file): before anything reads
// them, the files the PC offers on that port of the nxlink host are written
// beside the NRO. The stream is "SMSF", then per file a u16 name length, the
// name (a path under this folder), a u64 size and the bytes; a zero length
// ends it. Both ends are little-endian.
namespace {
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

void fetch_dev_files(const std::string& dir)
{
	int port = 0;
	for (int i = 1; __system_argv && i < __system_argc; i++)
		if (!strncmp(__system_argv[i], "SMS_DEV_FETCH=", 14))
			port = atoi(__system_argv[i] + 14);
	if (!port || __nxlink_host.s_addr == 0)
		return;
	const int s = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr = __nxlink_host;
	char magic[4];
	if (s < 0 || connect(s, (sockaddr*)&addr, sizeof addr) != 0 || !recv_all(s, magic, 4) ||
	    memcmp(magic, "SMSF", 4) != 0) {
		fprintf(stderr, "[switch] dev fetch: no files from port %d (%s)\n", port, strerror(errno));
		if (s >= 0)
			close(s);
		return;
	}
	static char chunk[1 << 16];
	for (;;) {
		uint16_t len = 0;
		char name[256];
		uint64_t size = 0;
		if (!recv_all(s, &len, 2) || len == 0 || len >= sizeof name || !recv_all(s, name, len))
			break;
		name[len] = 0;
		if (!recv_all(s, &size, 8))
			break;
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
		fprintf(stderr, "[switch] dev fetch: %s, %llu bytes%s\n", name, (unsigned long long)size,
		        ok && safe ? "" : " (not written)");
		if (!ok)
			break;
	}
	send(s, "OK\n", 3, 0);
	close(s);
}
} // namespace

// NAME=value launch arguments (nxlink --args, tools/nxlink.sh) act as env.txt
// lines, and are taken out of argv: the port reads its other arguments as a
// disc path.
void take_env_arguments()
{
	if (!__system_argv)
		return;  // no arguments at all (see __wrap_main)
	int out = 1;
	for (int i = 1; i < __system_argc; i++) {
		char* a = __system_argv[i];
		const char* eq = strchr(a, '=');
		bool name = eq && eq != a && (isupper((unsigned char)a[0]) || a[0] == '_');
		for (const char* c = a; name && c < eq; c++)
			name = isupper((unsigned char)*c) || isdigit((unsigned char)*c) || *c == '_';
		if (!name) {
			__system_argv[out++] = a;
			continue;
		}
		*(char*)eq = 0;
		setenv(a, eq + 1, 1);
		fprintf(stderr, "[switch] argument: %s=%s\n", a, eq + 1);
	}
	__system_argc = out;
	__system_argv[out] = NULL;
}

// The image packed into sms-standalone.nro beside this NRO: a quick-to-send
// sms.nro (nxlink) uses its disc without a 1.4 GB image of its own (likewise
// sms-eclipse.nro and sms-eclipse-standalone.nro).
// Mounts the RomFS of the NRO at `path` as `name`:; false if it has none.
bool mount_nro_romfs(const std::string& path, const char* name)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	NroStart start;
	NroHeader header;
	NroAssetHeader assets;
	bool ok = fread(&start, sizeof start, 1, f) == 1 && fread(&header, sizeof header, 1, f) == 1 &&
	          header.magic == 0x304f524e && fseek(f, header.size, SEEK_SET) == 0 &&
	          fread(&assets, sizeof assets, 1, f) == 1 && assets.magic == 0x54455341 && assets.romfs.size;
	fclose(f);
	return ok && R_SUCCEEDED(romfsMountFromFsdev(path.c_str(), header.size + assets.romfs.offset, name));
}

std::string find_standalone_disc_image(const std::string& dir)
{
	const std::string path = dir + "/" SMS_SWITCH_APP "-standalone.nro";
	if (!mount_nro_romfs(path, "standalone"))
		return std::string();
	for (const char* name : {"standalone:/disc.iso", "standalone:/disc.gcm", "standalone:/disc.ciso"}) {
		struct stat st;
		if (stat(name, &st) == 0 && S_ISREG(st.st_mode)) {
			fprintf(stderr, "[switch] disc image from %s (%s)\n", path.c_str(), name);
			return name;
		}
	}
	romfsUnmount("standalone");
	return std::string();
}

// prompts/switch.png from the program's copy (CMakeLists.txt), if missing.
void write_prompts(const std::string& dir)
{
	const std::string path = dir + "/switch.png";
	struct stat st;
	if (stat(path.c_str(), &st) == 0)
		return;
	mkdir(dir.c_str(), 0777);
	if (FILE* f = fopen(path.c_str(), "wb")) {
		fwrite(sms_switch_prompts_png, 1, sms_switch_prompts_png_size, f);
		fclose(f);
	}
}

void log_layout()
{
	const LowMemoryInfo* m = sms_switch_low_memory();
	fprintf(stderr, "[switch] image at 0x%lx (%lu KiB)%s, loaded at 0x%lx\n", (unsigned long)m->image_base,
	        (unsigned long)(m->image_size >> 10), m->image_moved ? ", moved below 4 GiB" : "",
	        (unsigned long)m->loaded_at);
	for (int i = 0; i < m->zone_count; i++)
		fprintf(stderr, "[switch] low window 0x%lx-0x%lx\n", (unsigned long)m->zones[i].base,
		        (unsigned long)(m->zones[i].base + m->zones[i].size));
	fprintf(stderr, "[switch] heap 0x%lx, %lu MiB\n", (unsigned long)m->heap_base,
	        (unsigned long)(m->heap_size >> 20));
	if (m->image_base + m->image_size > 0x100000000ULL)
		fprintf(stderr, "[switch] warning: the program is above 4 GiB; the game will stop at its first "
		                "pointer that does not fit. Start it again.\n");
}
} // namespace

// Before the game's constructors and main().
__attribute__((constructor(101))) static void switch_start(void)
{
	sms_switch_trace("[switch] start");
	char cwd[512];
	if (getcwd(cwd, sizeof cwd) && strncmp(cwd, "sdmc:/", 6) == 0 && strlen(cwd) > 6) {
		snprintf(g_dir_buf, sizeof g_dir_buf, "%s", cwd);
		for (size_t n = strlen(g_dir_buf); n > 6 && g_dir_buf[n - 1] == '/';)
			g_dir_buf[--n] = 0;
	}
	const std::string g_dir = g_dir_buf;
	mkdir(g_dir.c_str(), 0777);
	chdir(g_dir.c_str());

	extern struct in_addr __nxlink_host;
	if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
		g_nxlink_socket = nxlinkStdio();
	if (g_nxlink_socket < 0 && !freopen((g_dir + "/sms.log").c_str(), "w", stderr)) {
		char line[600];
		snprintf(line, sizeof line, "[switch] cannot write %s/sms.log (cwd %s): %s", g_dir.c_str(), cwd,
		         strerror(errno));
		sms_switch_trace(line);
	}
	setvbuf(stderr, NULL, _IONBF, 0);
	log_layout();

	if (g_nxlink_socket >= 0)
		fetch_dev_files(g_dir);
	load_env_file(g_dir + "/env.txt");
	take_env_arguments();
	// Gameplay at 60 fps unless settings.txt or env.txt asks for 30 (the
	// GameCube's, half the work per second) or 120.
	if (!getenv("SMS_FRAME_RATE") && !settings_has(g_dir, "frame_rate"))
		setenv("SMS_FRAME_RATE", "60", 1);
	// The screen is 16:9: a wider view, with the menus kept 4:3 in the
	// middle (widescreen = off in settings.txt for the original 4:3).
	if (!getenv("SMS_WIDESCREEN") && !settings_has(g_dir, "widescreen"))
		setenv("SMS_WIDESCREEN", "16:9", 1);
	// The gameplay HUD at the screen's edges (counters left, water gauge
	// right) rather than the 4:3 middle (widescreen_hud = centre for that).
	if (!getenv("SMS_WIDESCREEN_HUD") && !settings_has(g_dir, "widescreen_hud"))
		setenv("SMS_WIDESCREEN_HUD", "edges", 1);
	// The camera stick inverted on both axes (camera_invert_x / _y = 0 in
	// settings.txt, or SMS_CAMERA_INVERT_X=0 in env.txt, for the game's own).
	for (const char* axis : {"X", "Y"}) {
		const std::string name = std::string("SMS_CAMERA_INVERT_") + axis;
		const std::string key = std::string("camera_invert_") + char(axis[0] - 'A' + 'a');
		if (!getenv(name.c_str()) && !settings_has(g_dir, key.c_str()))
			setenv(name.c_str(), "1", 1);
	}
	// The game's A B X Y on the Switch buttons labelled so, as the prompts
	// show them. devkitPro's SDL maps them by position as on an Xbox pad
	// (a:b1, b:b0, x:b3, y:b2), which put the GameCube's A on the Switch's B.
	// SDL_GAMECONTROLLERCONFIG in env.txt replaces this mapping.
	setenv("SDL_GAMECONTROLLERCONFIG",
	       "000038f853776974636820436f6e7400,Switch Controller,a:b0,b:b1,back:b11,dpdown:b15,dpleft:b12,"
	       "dpright:b14,dpup:b13,leftshoulder:b6,leftstick:b4,lefttrigger:b8,leftx:a0,lefty:a1,"
	       "rightshoulder:b7,rightstick:b5,righttrigger:b9,rightx:a2,righty:a3,start:b10,x:b2,y:b3,",
	       0);
	// Button prompts as the Switch's buttons (button_prompts = gamecube in
	// settings.txt for the game's own), from prompts/switch.png beside the NRO,
	// written there from the program's copy when missing (replace it to
	// change them).
	if (!getenv("SMS_BUTTON_PROMPTS") && !settings_has(g_dir, "button_prompts"))
		setenv("SMS_BUTTON_PROMPTS", "switch", 1);
	if (!getenv("SMS_BUTTON_PROMPT_DIR") && !settings_has(g_dir, "button_prompt_dir")) {
		const std::string dir = g_dir + "/prompts";
		setenv("SMS_BUTTON_PROMPT_DIR", dir.c_str(), 1);
		write_prompts(dir);
	}
	// Mesa here advertises parallel shader compiles but compiles one at a
	// time: the warm-up's parallel mode (batches of 32 extra programs per
	// stage load) stalled a load for 6 s. Serial: a stage's own programs, and
	// extras only within 250 ms.
	setenv("SMS_GX_SHADER_WARMUP_SERIAL", "1", 0);
	// HD texture packs (mods/textures here): at most 1 GiB resident, not the
	// PC's 1.5 GiB; the console has about 3 GiB for everything.
	if (!getenv("SMS_TEXTURE_PACK_MB") && !settings_has(g_dir, "texture_pack_mb"))
		setenv("SMS_TEXTURE_PACK_MB", "1024", 1);
	sms_switch_spread_main_thread();
	sms_switch_start_clocks();
	if (applet_mode()) {
		const char* allow = getenv("SMS_SWITCH_ALLOW_APPLET");
		fprintf(stderr, "[switch] started in applet mode (applet type %d)%s\n", (int)appletGetAppletType(),
		        allow && *allow == '1' ? "; going on (SMS_SWITCH_ALLOW_APPLET=1)" : "; start it through a game (hold R)");
		if (!(allow && *allow == '1')) {
			explain_applet_mode();
			exit(1);
		}
	}
	setenv("HOME", g_dir.c_str(), 0);
	// The whole screen, at the console's resolution (720p handheld, 1080p
	// docked); the game's 4:3 picture is centred, or wide with widescreen.
	setenv("SMS_FULLSCREEN", "1", 0);
	setenv("XDG_DATA_HOME", (g_dir + "/save").c_str(), 0);
	setenv("XDG_CACHE_HOME", (g_dir + "/cache").c_str(), 0);
#ifdef SMS_SWITCH_DKGL
	// the deko3d renderer's compiled programs (SMS_SWITCH_RENDERER=deko3d):
	// the pack of the known ones (tools/dkgl/make-pack), then those made here
	dkgl::setShaderCache((g_dir + "/cache/dkgl").c_str(), (g_dir + "/dkgl-programs.pack").c_str());
#endif
	if (!getenv("SMS_DISC_IMAGE") && !getenv("SMS_DISC_ROOT")) {
		// sms-standalone.nro carries the image in its RomFS; else the SD card.
		std::string disc = find_bundled_disc_image();
		if (disc.empty())
			disc = find_disc_image();
		if (disc.empty())
			disc = find_standalone_disc_image(g_dir);
		if (!disc.empty())
			setenv("SMS_DISC_IMAGE", disc.c_str(), 1);
		else
			fprintf(stderr, "[switch] no disc image (.iso, .gcm, .ciso) in %s or %s/rom\n", g_dir.c_str(),
			        g_dir.c_str());
	}
	// An HD texture pack packed with the image (SMS_SWITCH_BUNDLE_TEXTURES),
	// read after one on the SD card (mods/textures/, the PC port's place).
	// A program without one of its own (Super Mario Eclipse) reads the one in
	// Sunshine's sms-standalone.nro, beside it or in sdmc:/switch/sms/: the
	// same textures, without a second 1.4 GB copy on the card.
	if (!getenv("SMS_TEXTURE_PACKS")) {
		auto is_dir = [](const char* dir) {
			struct stat st;
			return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
		};
		std::string found;
		for (const char* dir : {"romfs:/textures", "standalone:/textures"})
			if (found.empty() && is_dir(dir))
				found = dir;
		if (found.empty() && strcmp(SMS_SWITCH_APP, "sms") != 0)
			for (const std::string& nro : {g_dir + "/sms-standalone.nro", std::string("sdmc:/switch/sms/sms-standalone.nro")})
				if (found.empty() && mount_nro_romfs(nro, "sunshine")) {
					if (is_dir("sunshine:/textures"))
						found = "sunshine:/textures";
					else
						romfsUnmount("sunshine");
				}
		if (!found.empty()) {
			setenv("SMS_TEXTURE_PACKS", ("mods/textures;" + found).c_str(), 1);
			fprintf(stderr, "[switch] HD texture pack packed in %s\n", found.c_str());
		}
	}
}

// The game's threads keep running until the process ends, and the low windows
// alias the loader's heap, so the program does not return to the homebrew
// loader: it ends the process, as closing a game does.
extern "C" void __appExit(void);
extern "C" void NX_NORETURN __libnx_exit(int rc)
{
	char line[64];
	snprintf(line, sizeof line, "[switch] exit %d", rc);
	sms_switch_trace(line);
	fflush(NULL);
	if (g_nxlink_socket >= 0)
		close(g_nxlink_socket);
	__appExit();
	svcExitProcess();
}

// main, with an argument list even from a loader that passes none (Ryubing
// starts an NRO with argv NULL): just the program's name, as the port
// expects (-Wl,--wrap=main).
extern "C" int __real_main(int argc, char** argv);
extern "C" int __wrap_main(int argc, char** argv)
{
	if (!argv) {
		static char name[] = SMS_SWITCH_APP ".nro";
		static char* args[2] = {name, NULL};
		return __real_main(1, args);
	}
	return __real_main(argc, argv);
}
