/* ucontext.h for the Switch: Horizon delivers no signals, so the port's crash
 * handler never receives a context; this only lets it compile. */
#ifndef SMS_SWITCH_UCONTEXT_H
#define SMS_SWITCH_UCONTEXT_H
#include <signal.h>
typedef struct {
	unsigned long long regs[31], sp, pc, pstate, fault_address;
} mcontext_t;
typedef struct ucontext_t {
	struct ucontext_t* uc_link;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
} ucontext_t;
#endif
