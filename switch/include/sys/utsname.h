/* sys/utsname.h for the Switch (switch/src/posix_shims.cpp). */
#ifndef SMS_SWITCH_SYS_UTSNAME_H
#define SMS_SWITCH_SYS_UTSNAME_H
#ifdef __cplusplus
extern "C" {
#endif
struct utsname {
	char sysname[65], nodename[65], release[65], version[65], machine[65];
};
int uname(struct utsname* name);
#ifdef __cplusplus
}
#endif
#endif
