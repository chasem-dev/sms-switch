/* sys/uio.h for the Switch: the type only (newlib has no readv). */
#ifndef SMS_SWITCH_SYS_UIO_H
#define SMS_SWITCH_SYS_UIO_H
#include <stddef.h>
struct iovec {
	void* iov_base;
	size_t iov_len;
};
#endif
