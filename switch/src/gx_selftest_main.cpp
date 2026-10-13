// Entry point of gx_selftest.nro: the port's GX self-test
// (sms-port platform/gx/tests/gx_selftest.cpp) on the Switch's own GL stack,
// in a window, with its report in gx_selftest.log beside the NRO and the
// final frame in gx_selftest.ppm.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

extern int gx_selftest_main(int argc, char** argv);

int main(int, char**)
{
	freopen("gx_selftest.log", "w", stdout);
	dup2(fileno(stdout), fileno(stderr));
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
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
