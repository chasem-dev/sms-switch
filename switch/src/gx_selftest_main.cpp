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
#include "dev_link.h"
#ifdef SMS_SWITCH_DKGL
#include "dkgl/dkgl.h"
#endif

extern int gx_selftest_main(int argc, char** argv);

int main(int, char**)
{
	// sent with tools/switch-dev.py or nxlink -s: the report goes back over
	// the network, as it runs
	if (sms_dev_link_start(".") >= 0) {
	} else if (__nxlink_host.s_addr && R_SUCCEEDED(socketInitializeDefault()) && nxlinkStdio() >= 0) {
	} else {
		freopen("gx_selftest.log", "w", stdout);
		dup2(fileno(stdout), fileno(stderr));
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
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
