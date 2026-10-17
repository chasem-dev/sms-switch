// The PC's line into a test run (tools/switch-dev.py run; dev_link.cpp).
#pragma once
#include <string>

// SMS_DEV_LISTEN=<port> among the launch arguments: waits for the PC on that
// port, writes the files it sends under dir, and makes the connection stdout
// and stderr. Returns the socket, or -1 (no such argument, or no PC came).
int sms_dev_link_start(const std::string& dir);
