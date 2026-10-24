// What libretro-common's rthreads.c calls that xsdk's pthreads lack: thread attributes for a scheduling policy
// (docs/GUIDE.md, "Porting: the C and POSIX calls"). rthreads.c sets them only for a priority, which sthread_create
// never asks for, and doesn't look at what they return.

#include <errno.h>
#include <pthread.h>
#include <sched.h>

int pthread_attr_setschedpolicy(pthread_attr_t *attribute, int policy)
{
	(void)attribute;
	(void)policy;
	return ENOSYS;
}

int pthread_attr_setschedparam(pthread_attr_t *attribute, const struct sched_param *parameter)
{
	(void)attribute;
	(void)parameter;
	return ENOSYS;
}
