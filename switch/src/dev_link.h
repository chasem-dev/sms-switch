// The PC's line into a test run (tools/switch-dev.py run; dev_link.cpp).
#pragma once
#include <string>

// SMS_DEV_LISTEN=<port> among the launch arguments: waits for the PC on that
// port, writes the files it sends under dir, and makes the connection stdout
// and stderr. Returns the socket, or -1 (no such argument, or no PC came).
int sms_dev_link_start(const std::string& dir);

// Before the process ends: stdout and stderr off the network (the log
// socket, from either sms_dev_link_start or nxlinkStdio) and every link
// socket shut down and closed, so no socket is left mid-call. A process that
// ended (or crashed closing them after socketExit) with them open has taken
// the console's bsdsocket service, and with it the whole system, down.
void sms_dev_link_end(int stdio_socket);
