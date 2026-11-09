/* Loaded into SpringBoard: report the device as activated and never ask for the setup assistant. */
#include <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

#define LOCKDOWN_ACTIVATED 2 /* int at +8 of SBLockdownManager: 2 = activated */

static void (*orig_reason)(id, SEL, long);

static void
act_state(id self, SEL _cmd)
{
	*(int *)((char *)self + 8) = LOCKDOWN_ACTIVATED;
}

static void
set_reason(id self, SEL _cmd, long reason)
{
	orig_reason(self, _cmd, 0);
}

__attribute__((constructor)) static void
sbfix_init(void)
{
	Class c = objc_getClass("SBLockdownManager");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("_setupActivationState")) : NULL;
	if (m) {
		method_setImplementation(m, (IMP)act_state);
	}
	Class s = objc_getClass("SBSetupManager");
	Method r = s ? class_getInstanceMethod(s, sel_registerName("_setSetupRequiredReason:")) : NULL;
	if (r) {
		orig_reason = (void (*)(id, SEL, long))method_getImplementation(r);
		method_setImplementation(r, (IMP)set_reason);
	}
	fprintf(stderr, "sbfix: pid %d lockdown %s setup %s\n", getpid(), m ? "patched" : "MISSING", r ? "patched" : "MISSING");
}
/* rebuild */
