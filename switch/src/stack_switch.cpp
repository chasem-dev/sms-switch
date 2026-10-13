// Runs a thread's game code on its low stack (PORT_LOW_STACK_CALL in the
// port; the AArch64 counterpart of sms-port platform/os/windows_stack.cpp).
// libnx gives every thread a stack mirrored at an address of its own, above
// 4 GiB, while the game keeps pointers to its locals in 4-byte fields.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "port_win64_stack.h"

namespace {
// Where a thread leaving its low stack resumes: the original stack pointer
// inside sms_switch_stack_enter, below the registers it saved.
struct ExitPoint {
	void* sp;
};
thread_local ExitPoint* exit_point;
} // namespace

// x0=top, x1=entry, x2=argument, x3=exit point. Saves the callee-saved
// registers (x19-x30, d8-d15) on the original stack, records that stack
// pointer for a thread exit, and keeps it in x19 across the call.
extern "C" void* sms_switch_stack_enter(void*, void* (*)(void*), void*, ExitPoint*);
extern "C" __attribute__((noreturn)) void sms_switch_stack_leave(ExitPoint*);
__asm__(
	".text\n"
	".global sms_switch_stack_enter\n"
	".type sms_switch_stack_enter, %function\n"
	"sms_switch_stack_enter:\n"
	"  stp x29, x30, [sp, #-160]!\n"
	"  mov x29, sp\n"
	"  stp x19, x20, [sp, #16]\n"
	"  stp x21, x22, [sp, #32]\n"
	"  stp x23, x24, [sp, #48]\n"
	"  stp x25, x26, [sp, #64]\n"
	"  stp x27, x28, [sp, #80]\n"
	"  stp d8, d9, [sp, #96]\n"
	"  stp d10, d11, [sp, #112]\n"
	"  stp d12, d13, [sp, #128]\n"
	"  stp d14, d15, [sp, #144]\n"
	"  mov x9, sp\n"
	"  str x9, [x3]\n"
	"  mov x19, sp\n"
	"  and x0, x0, #-16\n"
	"  mov sp, x0\n"
	"  mov x0, x2\n"
	"  blr x1\n"
	"  mov sp, x19\n"
	"sms_switch_stack_restore:\n"
	"  ldp d14, d15, [sp, #144]\n"
	"  ldp d12, d13, [sp, #128]\n"
	"  ldp d10, d11, [sp, #112]\n"
	"  ldp d8, d9, [sp, #96]\n"
	"  ldp x27, x28, [sp, #80]\n"
	"  ldp x25, x26, [sp, #64]\n"
	"  ldp x23, x24, [sp, #48]\n"
	"  ldp x21, x22, [sp, #32]\n"
	"  ldp x19, x20, [sp, #16]\n"
	"  ldp x29, x30, [sp], #160\n"
	"  ret\n"
	// x0=exit point: back onto the original stack, returning NULL
	".global sms_switch_stack_leave\n"
	".type sms_switch_stack_leave, %function\n"
	"sms_switch_stack_leave:\n"
	"  ldr x9, [x0]\n"
	"  mov sp, x9\n"
	"  mov x0, #0\n"
	"  b sms_switch_stack_restore\n");

void* port_win64_stack_call(void* stack, size_t size, void* (*entry)(void*), void* argument)
{
	if (!stack || size < 65536 || (uintptr_t)stack + size > 0x80000000ULL) {
		fprintf(stderr, "[port] invalid low stack %p+%zu\n", stack, size);
		abort();
	}
	ExitPoint exit = {NULL};
	ExitPoint* previous = exit_point;
	exit_point = &exit;
	void* result = sms_switch_stack_enter((char*)stack + size, entry, argument, &exit);
	exit_point = previous;
	return result;
}

void port_win64_thread_exit(void)
{
	if (!exit_point) {
		fprintf(stderr, "[port] thread exit outside a low stack\n");
		abort();
	}
	// Return within this low stack first; the thread then ends normally
	// from its own entry function on its own stack.
	sms_switch_stack_leave(exit_point);
}
