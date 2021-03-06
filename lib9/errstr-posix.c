#include "lib9.h"

static char errstring[ERRLEN];

enum
{
	Magic = 0xffffff
};

void
werrstr(char *fmt, ...)
{
	extern int errno;
	va_list arg;

	va_start(arg, fmt);
	doprint(errstring, errstring+sizeof(errstring), fmt, arg);
	va_end(arg);
	errno = Magic;
}

void
oserrstr(char *buf)
{
	strncpy(buf, strerror(errno), ERRLEN);
}

int
errstr(char *buf)
{
	extern int errno;
	extern int sys_nerr;
	
	char *p;

	if(errno == Magic)
		strncpy(buf, errstring, ERRLEN);
	else
		oserrstr(buf);
	return 1;
}
