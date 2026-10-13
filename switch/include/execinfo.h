/* execinfo.h for the Switch: frame-pointer walk (switch/src/posix_shims.cpp). */
#ifndef SMS_SWITCH_EXECINFO_H
#define SMS_SWITCH_EXECINFO_H
#ifdef __cplusplus
extern "C" {
#endif
int backtrace(void** frames, int capacity);
char** backtrace_symbols(void* const* frames, int count);
void backtrace_symbols_fd(void* const* frames, int count, int fd);
#ifdef __cplusplus
}
#endif
#endif
