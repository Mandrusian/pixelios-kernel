/* Loaded into SpringBoard: activation fix plus a periodic state dump on stderr (the console). */
#include <dispatch/dispatch.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <unistd.h>

#define LOCKDOWN_ACTIVATED 2 /* int at +8 of SBLockdownManager: 2 = activated */
#define S(t, o, n) ((t (*)(id, SEL))objc_msgSend)((id)(o), sel_registerName(n))
#define OK(o, n) ((o) && class_respondsToSelector(object_getClass((id)(o)), sel_registerName(n)))

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

static const char *
desc(id o)
{
	id d = o ? S(id, o, "description") : NULL;
	return d ? S(const char *, d, "UTF8String") : "(nil)";
}

static void
singleton(const char *cls, const char *sel)
{
	Class c = objc_getClass(cls);
	id o = c && class_getClassMethod(c, sel_registerName(sel)) ? S(id, c, sel) : NULL;
	fprintf(stderr, "sbdiag: %s = %.600s\n", cls, o ? desc(o) : "(none)");
	if (OK(o, "isUILocked")) {
		fprintf(stderr, "sbdiag:   isUILocked %d\n", S(int, o, "isUILocked"));
	}
}

static void
dump(void)
{
	Class ac = objc_getClass("UIApplication");
	id app = ac ? S(id, ac, "sharedApplication") : NULL;
	id wins = app ? S(id, app, "windows") : NULL;
	long n = wins ? S(long, wins, "count") : 0;
	fprintf(stderr, "sbdiag: ---- %ld windows\n", n);
	for (long i = 0; i < n; i++) {
		id w = ((id (*)(id, SEL, long))objc_msgSend)(wins, sel_registerName("objectAtIndex:"), i);
		struct { double x, y, w, h; } f = ((typeof(f) (*)(id, SEL))objc_msgSend)(w, sel_registerName("frame"));
		id layer = S(id, w, "layer"), subs = S(id, layer, "sublayers"), rvc = S(id, w, "rootViewController");
		fprintf(stderr, "sbdiag: win %ld %s %.0f,%.0f %.0fx%.0f hidden %d alpha %.2f level %.0f sublayers %ld rvc %s\n", i,
		    object_getClassName(w), f.x, f.y, f.w, f.h, S(int, w, "isHidden"), S(double, w, "alpha"),
		    S(double, w, "windowLevel"), subs ? S(long, subs, "count") : 0L, rvc ? object_getClassName(rvc) : "-");
	}
	singleton("SBLockScreenManager", "sharedInstance");
	singleton("SBMainWorkspace", "sharedInstance");
	singleton("SBCoverSheetPresentationManager", "sharedInstance");
	singleton("SBBacklightController", "sharedInstance");
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
	fprintf(stderr, "sbdiag: pid %d lockdown %s setup %s\n", getpid(), m ? "patched" : "MISSING", r ? "patched" : "MISSING");
	dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_SEC), 60 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(t, ^{ dump(); });
	dispatch_resume(t);
}
