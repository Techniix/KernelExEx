/* Host-side stand-in for <windows.h>, just enough to build util/prep on
 * Linux: FindFirstFile over one directory with a case-insensitive pattern,
 * and _stat. Paths may use either slash. */
#ifndef KEX_HOSTSHIM_WINDOWS_H
#define KEX_HOSTSHIM_WINDOWS_H
#include <dirent.h>
#include <fnmatch.h>
#include <string.h>
#include <sys/stat.h>
#include <string>

typedef struct { char cFileName[260]; } WIN32_FIND_DATA;
struct kex_find { DIR* d; std::string pat; };
typedef kex_find* HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)0)

static inline std::string kex_path(const char* p)
{
	std::string s(p);
	for (size_t i = 0; i < s.size(); i++) if (s[i] == '\\') s[i] = '/';
	return s;
}
static inline bool FindNextFile(HANDLE h, WIN32_FIND_DATA* fd)
{
	struct dirent* e;
	while ((e = readdir(h->d)) != 0)
		if (fnmatch(h->pat.c_str(), e->d_name, FNM_CASEFOLD) == 0)
		{
			strncpy(fd->cFileName, e->d_name, sizeof(fd->cFileName) - 1);
			fd->cFileName[sizeof(fd->cFileName) - 1] = 0;
			return true;
		}
	return false;
}
static inline void FindClose(HANDLE h) { closedir(h->d); delete h; }
static inline HANDLE FindFirstFile(const char* pattern, WIN32_FIND_DATA* fd)
{
	std::string p = kex_path(pattern), dir = ".";
	size_t slash = p.rfind('/');
	if (slash != std::string::npos) { dir = p.substr(0, slash); p = p.substr(slash + 1); }
	DIR* d = opendir(dir.c_str());
	if (!d) return INVALID_HANDLE_VALUE;
	HANDLE h = new kex_find;
	h->d = d;
	h->pat = p;
	if (!FindNextFile(h, fd)) { FindClose(h); return INVALID_HANDLE_VALUE; }
	return h;
}
#define _stat stat
#include <unistd.h>
static inline bool SetCurrentDirectory(const char* p) { return chdir(kex_path(p).c_str()) == 0; }
#endif
