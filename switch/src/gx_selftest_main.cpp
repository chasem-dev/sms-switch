// Entry point of gx_selftest.nro: the port's GX self-test
// (sms-port platform/gx/tests/gx_selftest.cpp) on the Switch's own GL stack,
// in a window, with its report in gx_selftest.log beside the NRO and the
// final frame in gx_selftest.ppm.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <switch.h>
#include <string>
#include "dev_link.h"
#ifdef SMS_SWITCH_DKGL
#include "dkgl/dkgl.h"
#endif

extern int gx_selftest_main(int argc, char** argv);

extern "C" int __system_argc;
extern "C" char** __system_argv;

static int g_link = -1;  // stdout and stderr's socket, if they go to the PC

// low_memory.cpp moved the program over the loader's heap, so it cannot go
// back to the homebrew menu: the process ends, its log socket closed first
// (as the game's does, switch_main.cpp). Returning crashed it, with the
// socket open, and the console's bsdsocket service after it.
extern "C" void __appExit(void);
extern "C" void NX_NORETURN __libnx_exit(int)
{
	fflush(NULL);
	sms_dev_link_end(g_link);
	__appExit();
	svcExitProcess();
}

int main(int, char**)
{
	// sent with tools/switch-dev.py or nxlink -s: the report goes back over
	// the network, as it runs
	if ((g_link = sms_dev_link_start(".")) >= 0) {
	} else if (__nxlink_host.s_addr && R_SUCCEEDED(socketInitializeDefault()) && (g_link = nxlinkStdio()) >= 0) {
	} else {
		freopen("gx_selftest.log", "w", stdout);
		dup2(fileno(stdout), fileno(stderr));
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	// NAME=value launch arguments, as the game takes them (SMS_SWITCH_RENDERER=gl
	// instead of gx_selftest.gl, SMS_DKGL_DUMP=- ...)
	for (int i = 1; __system_argv && i < __system_argc; i++)
		if (const char* eq = strchr(__system_argv[i], '='))
			if (eq != __system_argv[i] && strncmp(__system_argv[i], "SMS_DEV_", 8) != 0) {
				setenv(std::string(__system_argv[i], size_t(eq - __system_argv[i])).c_str(), eq + 1, 1);
				printf("gx_selftest: %s\n", __system_argv[i]);
			}
#ifdef SMS_SWITCH_DKGL
	// on the deko3d renderer, unless gx_selftest.gl is beside it (Mesa's GL)
	if (access("gx_selftest.gl", F_OK) != 0) setenv("SMS_SWITCH_RENDERER", "deko3d", 0);
	setenv("SMS_DKGL_TRACE", "400", 0);  // the first draws' state in the log
	dkgl::setShaderCache("gx_selftest-dkgl", nullptr);
	printf("gx_selftest: renderer %s\n", getenv("SMS_SWITCH_RENDERER") ? getenv("SMS_SWITCH_RENDERER") : "gl");
	// dkgl's own checks first: the GL paths sms_gx's draws take, one at a time
	if (getenv("SMS_SWITCH_RENDERER") && !strcmp(getenv("SMS_SWITCH_RENDERER"), "deko3d") && dkgl::init(1920, 1080))
		dkgl::runChecks();
#endif
	char* args[] = {(char*)"gx_selftest", (char*)"--window", (char*)"gx_selftest.ppm", NULL};
	const int rc = gx_selftest_main(3, args);
	printf("gx_selftest: exit %d\n", rc);
	return rc;
}

// The port's PTR32 slots report addresses above 4 GiB here (port_runtime.cpp
// in the game itself).
extern "C" void port_ptr32_trap(const void* p)
{
	printf("PTR32: address %p does not fit a 32-bit slot\n", p);
	fflush(stdout);
	abort();
}
