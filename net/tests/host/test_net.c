/*
 *  KernelEx net: host tests for the portable parts of kextls and winhttp.
 *  Licensed under the GNU General Public License version 2.
 *
 *    test_net unit
 *    test_net get <url> [--proxy host:port] [--head] [--expect-fail]
 *        prints one line: OK status=<n> proto=<p> cipher=<c> bytes=<n>
 *                     or: FAIL whttp=<n> tls=<n> <detail>
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kextls.h"
#include "whttp_core.h"
#include "w9xrt.h"

void kextls_last_entropy_report(int *cryptoapi, int *jitter_distinct);
void kextls_platform_init(void);

typedef unsigned long long u64;
typedef long long s64;
u64 kexdiv___udivdi3(u64, u64);
u64 kexdiv___umoddi3(u64, u64);
s64 kexdiv___divdi3(s64, s64);
s64 kexdiv___moddi3(s64, s64);

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static u64 rng64(void)
{
	static u64 x = 88172645463325252ULL;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return x;
}

static void test_division(void)
{
	int i;
	printf("division helpers\n");
	for (i = 0; i < 200000; i++)
	{
		u64 a = rng64(), b = rng64() >> (rng64() % 64);
		s64 sa = (s64)rng64(), sb = (s64)(rng64() >> (rng64() % 63));
		if (b == 0)
			b = 1;
		if (sb == 0)
			sb = -3;
		if (i % 3 == 0)
			a >>= 33;
		CHECK(kexdiv___udivdi3(a, b) == a / b, "udiv %llu/%llu", a, b);
		CHECK(kexdiv___umoddi3(a, b) == a % b, "umod %llu %% %llu", a, b);
		CHECK(kexdiv___divdi3(sa, sb) == sa / sb, "sdiv %lld/%lld", sa, sb);
		CHECK(kexdiv___moddi3(sa, sb) == sa % sb, "smod %lld %% %lld", sa, sb);
	}
}

static void test_gmtime(void)
{
	int i;
	printf("gmtime_r\n");
	for (i = 0; i < 200000; i++)
	{
		long long t = (long long)(rng64() % 20000000000ULL) - 2000000000LL;
		time_t tt = (time_t)t;
		struct tm a, b;
		w9x_gmtime_r(&t, &a);
		gmtime_r(&tt, &b);
		CHECK(a.tm_year == b.tm_year && a.tm_mon == b.tm_mon && a.tm_mday == b.tm_mday &&
		      a.tm_hour == b.tm_hour && a.tm_min == b.tm_min && a.tm_sec == b.tm_sec &&
		      a.tm_wday == b.tm_wday && a.tm_yday == b.tm_yday, "t=%lld", t);
		if (failures > 5)
			return;
	}
}

static void test_url(void)
{
	whttp_url u;
	printf("URL parsing\n");
	CHECK(whttp_parse_url("https://example.com", &u) == 0 && u.secure && u.port == 443 &&
	      !strcmp(u.host, "example.com") && !strcmp(u.path, "/"), "basic https");
	CHECK(whttp_parse_url("http://a.b:8080/x/y?z=1#frag", &u) == 0 && !u.secure && u.port == 8080 &&
	      !strcmp(u.path, "/x/y?z=1"), "port, query, fragment");
	CHECK(whttp_parse_url("HTTPS://Host?q", &u) == 0 && !strcmp(u.path, "/?q"), "query without path");
	CHECK(whttp_parse_url("https://user:pw@host/", &u) != 0, "userinfo rejected");
	CHECK(whttp_parse_url("https://[::1]/", &u) != 0, "IPv6 literal rejected");
	CHECK(whttp_parse_url("https://host:0/", &u) != 0, "port 0 rejected");
	CHECK(whttp_parse_url("https://host:70000/", &u) != 0, "port range");
	CHECK(whttp_parse_url("ftp://host/", &u) != 0, "scheme");
	CHECK(whttp_parse_url("https://ho st/", &u) != 0, "space in host");
}

static void test_drbg(void)
{
	static unsigned char a[1 << 20], b[64];
	unsigned long counts[256];
	double chi = 0, expect;
	int i, capi, jitter;

	printf("DRBG\n");
	memset(counts, 0, sizeof(counts));
	CHECK(kextls_random(a, sizeof(a)) == KEXTLS_OK, "1 MB request");
	CHECK(kextls_random(b, sizeof(b)) == KEXTLS_OK, "small request");
	CHECK(memcmp(a, b, sizeof(b)) != 0, "streams differ");
	for (i = 0; i < (int)sizeof(a); i++)
		counts[a[i]]++;
	expect = sizeof(a) / 256.0;
	for (i = 0; i < 256; i++)
		chi += (counts[i] - expect) * (counts[i] - expect) / expect;
	/* 255 degrees of freedom: p=0.001 bounds are roughly 190 and 330 */
	CHECK(chi > 180 && chi < 340, "byte distribution chi^2 = %.1f", chi);
	kextls_last_entropy_report(&capi, &jitter);
	CHECK(capi, "entropy source 1 reported");
	printf("  chi^2=%.1f, entropy: system source=%d jitter distinct=%d\n", chi, capi, jitter);
}

static int unit(void)
{
	test_division();
	test_gmtime();
	test_url();
	test_drbg();
	printf(failures ? "UNIT FAILED (%d)\n" : "UNIT OK\n", failures);
	return failures ? 1 : 0;
}

static int get(int argc, char **argv)
{
	const char *url = argv[2], *proxy = NULL;
	char proxy_host[256] = "", detail[160], proto[16] = "", cipher[64] = "", body[4096];
	unsigned short proxy_port = 0;
	int i, e = 0, tls = 0, head = 0, n;
	long long total = 0;
	whttp_url u;
	whttp_timeouts t = { 10000, 10000, 15000 };
	whttp_conn *c;
	whttp_response r;
	char target[2400];

	for (i = 3; i < argc; i++)
	{
		if (!strcmp(argv[i], "--proxy") && i + 1 < argc)
			proxy = argv[++i];
		else if (!strcmp(argv[i], "--head"))
			head = 1;
	}
	if (whttp_parse_url(url, &u) != 0)
	{
		printf("FAIL whttp=%d tls=0 bad url\n", WHTTP_E_URL);
		return 1;
	}
	if (proxy)
	{
		const char *colon = strrchr(proxy, ':');
		size_t hl = colon ? (size_t)(colon - proxy) : strlen(proxy);
		memcpy(proxy_host, proxy, hl);
		proxy_host[hl] = '\0';
		proxy_port = (unsigned short)(colon ? atoi(colon + 1) : 8080);
	}
	c = whttp_conn_open(u.host, u.port, u.secure, proxy ? proxy_host : NULL, proxy_port, &t, &e, &tls, detail,
	                    sizeof(detail));
	if (!c)
	{
		printf("FAIL whttp=%d tls=%d %s\n", e, tls, detail);
		return 1;
	}
	whttp_conn_tls_info(c, proto, sizeof(proto), cipher, sizeof(cipher));
	if (proxy && !u.secure)
		snprintf(target, sizeof(target), "http://%s:%u%s", u.host, (unsigned)u.port, u.path);
	else
		snprintf(target, sizeof(target), "%s", u.path);
	e = whttp_send_request(c, head ? "HEAD" : "GET", target, u.host, "User-Agent: kextls-test/1\r\nAccept: */*\r\n",
	                       NULL, 0, -1);
	if (e == 0)
		e = whttp_read_response(c, head ? "HEAD" : "GET", &r);
	if (e != 0)
	{
		printf("FAIL whttp=%d tls=%d request\n", e, whttp_conn_tls_error(c));
		whttp_conn_close(c);
		return 1;
	}
	while ((n = whttp_read_body(c, &r, body, sizeof(body))) > 0)
		total += n;
	if (n < 0)
	{
		printf("FAIL whttp=%d tls=%d body after %lld bytes\n", n, whttp_conn_tls_error(c), total);
		whttp_response_free(&r);
		whttp_conn_close(c);
		return 1;
	}
	printf("OK status=%d proto=%s cipher=%s bytes=%lld chunked=%d keepalive=%d\n", r.status, proto[0] ? proto : "-",
	       cipher[0] ? cipher : "-", total, r.chunked, r.keep_alive);
	whttp_response_free(&r);
	whttp_conn_close(c);
	return 0;
}

int main(int argc, char **argv)
{
	kextls_platform_init();
	if (argc >= 2 && !strcmp(argv[1], "unit"))
		return unit();
	if (argc >= 3 && !strcmp(argv[1], "get"))
		return get(argc, argv);
	fprintf(stderr, "usage: test_net unit | get <url> [--proxy host:port] [--head]\n");
	return 2;
}
