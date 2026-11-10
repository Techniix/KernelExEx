/* See kexcrt_pre.h. */
#include <stdarg.h>
#include <stddef.h>

int kexcrt_vsnprintf(char *buffer, size_t n, const char *format, va_list ap);

int __ms_vsnprintf(char *buffer, size_t n, const char *format, va_list ap)
{
	return kexcrt_vsnprintf(buffer, n, format, ap);
}

int __ms_snprintf(char *buffer, size_t n, const char *format, ...)
{
	va_list ap;
	int rv;
	va_start(ap, format);
	rv = kexcrt_vsnprintf(buffer, n, format, ap);
	va_end(ap);
	return rv;
}

/* out-of-line copies under the standard names (C++ callers, function pointers) */
int vsnprintf(char *buffer, size_t n, const char *format, va_list ap)
{
	return kexcrt_vsnprintf(buffer, n, format, ap);
}

int snprintf(char *buffer, size_t n, const char *format, ...)
{
	va_list ap;
	int rv;
	va_start(ap, format);
	rv = kexcrt_vsnprintf(buffer, n, format, ap);
	va_end(ap);
	return rv;
}
