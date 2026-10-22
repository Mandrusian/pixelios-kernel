/* Minimal memcpy, memmove, memset, strlen, strchr for the freestanding shim (XNU uses the kernel libc). */
#include <stddef.h>
#pragma GCC optimize("no-tree-loop-distribute-patterns")
void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	while (n--)
		*p++ = *q++;
	return d;
}
void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	if (p < q)
		while (n--)
			*p++ = *q++;
	else
		while (n--)
			p[n] = q[n];
	return d;
}
void *memset(void *d, int c, size_t n)
{
	unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}
size_t strlen(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}
char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return 0;
	}
}
