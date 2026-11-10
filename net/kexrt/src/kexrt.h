/* KernelEx kexrt: internal declarations. GPL version 2. */
#ifndef KEXRT_H
#define KEXRT_H

typedef struct
{
	const char *name;
	unsigned long addr;
} kexrt_api;

extern const kexrt_api kexrt_kernel32_apis[];
extern const int kexrt_kernel32_count;
extern const kexrt_api kexrt_ws2_32_apis[];
extern const int kexrt_ws2_32_count;

void kexrt_fls_thread_exit(void);
void kx_sync_thread_detach(void);

#endif
