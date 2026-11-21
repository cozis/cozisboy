// cozisboy web: a small HTTP server for managing the device from a browser.
//
// - Pairing: a 6-digit code is generated at start (printed on stderr and
//   written to /run/cozisboy-web.code so the launcher can show it). The browser
//   exchanges it for a session cookie. 5 wrong codes -> a new code.
// - Games, battery saves, save states and covers through the game library
//   (common/library.h): no paths or files are exposed.
// - Status (battery, storage, Wi-Fi, uptime), brightness, live screenshot.
//
// Plain HTTP on the local network, one thread per connection, no keep-alive.
// Usage: cozisboy-web [port]   (default 80)

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/fb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "library.h"

#define ROOT "/data"
#define WWW "/usr/share/cozisboy-web"
#define CODE_FILE "/run/cozisboy-web.code"
#define BACKLIGHT "/sys/class/backlight/backlight/"
#define BATTERY "/sys/class/power_supply/axp20x-battery/"
#define MAX_SESSIONS 16
#define MAX_FAILURES 5
#define HDR_MAX 8192

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char pair_code[7];
static char sessions[MAX_SESSIONS][33];
static int next_session, failures;
static const char *www_dir = WWW;
static int dev_noauth;

// ---------------------------------------------------------------- utilities

static void random_hex(char *out, size_t nbytes)
{
	unsigned char b[32];
	if (getrandom(b, nbytes, 0) != (ssize_t)nbytes)
		abort();
	for (size_t i = 0; i < nbytes; i++)
		sprintf(out + 2 * i, "%02x", b[i]);
}

// Caller holds the lock.
static void new_pair_code(void)
{
	uint32_t r;
	if (getrandom(&r, sizeof r, 0) != sizeof r)
		abort();
	snprintf(pair_code, sizeof pair_code, "%06u", r % 1000000);
	failures = 0;
	FILE *f = fopen(CODE_FILE, "w");
	if (f) {
		fprintf(f, "%s\n", pair_code);
		fclose(f);
	}
	fprintf(stderr, "cozisboy-web: pairing code %s\n", pair_code);
}

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= n;
	}
	return 0;
}

static const char *status_text(int code)
{
	switch (code) {
	case 200: return "OK";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 409: return "Conflict";
	case 411: return "Length Required";
	case 413: return "Payload Too Large";
	case 429: return "Too Many Requests";
	case 507: return "Insufficient Storage";
	default: return "Internal Server Error";
	}
}

static void respond(int fd, int code, const char *ctype, const char *extra,
		    const void *body, size_t len)
{
	char h[1024];
	int n = snprintf(h, sizeof h,
			 "HTTP/1.1 %d %s\r\n"
			 "Content-Type: %s\r\n"
			 "Content-Length: %zu\r\n"
			 "Cache-Control: no-store\r\n"
			 "X-Content-Type-Options: nosniff\r\n"
			 "Connection: close\r\n"
			 "%s\r\n",
			 code, status_text(code), ctype, len, extra ? extra : "");
	if (send_all(fd, h, n) == 0 && len)
		send_all(fd, body, len);
}

static void respond_error(int fd, int code, const char *msg)
{
	char b[256];
	int n = snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
	respond(fd, code, "application/json", NULL, b, n);
}

// Append a JSON string literal.
static void json_str(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s; s++) {
		unsigned char c = *s;
		if (c == '"' || c == '\\')
			fprintf(f, "\\%c", c);
		else if (c < 0x20)
			fprintf(f, "\\u%04x", c);
		else
			fputc(c, f);
	}
	fputc('"', f);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

// Find query parameter `name` in `query` and URL-decode it into out.
static int query_param(const char *query, const char *name, char *out, size_t outlen)
{
	size_t nl = strlen(name);
	const char *p = query;
	while (p && *p) {
		if (!strncmp(p, name, nl) && p[nl] == '=') {
			p += nl + 1;
			size_t o = 0;
			while (*p && *p != '&') {
				int c = *p++;
				if (c == '+')
					c = ' ';
				else if (c == '%' && hexval(p[0]) >= 0 && hexval(p[1]) >= 0) {
					c = hexval(p[0]) * 16 + hexval(p[1]);
					p += 2;
				}
				if (c == 0 || o + 1 >= outlen)
					return -1;
				out[o++] = c;
			}
			out[o] = 0;
			return 0;
		}
		p = strchr(p, '&');
		if (p)
			p++;
	}
	return -1;
}

static int read_sysfs(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static long sysfs_long(const char *path, long def)
{
	char b[32];
	return read_sysfs(path, b, sizeof b) ? def : atol(b);
}

// ----------------------------------------------------------------- request

struct req {
	int fd;
	char method[8];
	char path[1024];	// without query
	char query[2048];
	long long content_length;
	int has_xhdr;		// X-Cozis header (CSRF guard)
	int authed;
	char token[33];
	char *body;		// bytes of the body already read with the headers
	size_t body_have;
};

static int check_session(const char *cookie_hdr, char *tok_out)
{
	const char *p = strstr(cookie_hdr, "coz=");
	if (!p)
		return 0;
	p += 4;
	char tok[33];
	size_t i;
	for (i = 0; i < 32 && isxdigit((unsigned char)p[i]); i++)
		tok[i] = p[i];
	tok[i] = 0;
	if (i != 32)
		return 0;
	int ok = 0;
	pthread_mutex_lock(&lock);
	for (int s = 0; s < MAX_SESSIONS; s++)
		if (sessions[s][0] && !strcmp(sessions[s], tok))
			ok = 1;
	pthread_mutex_unlock(&lock);
	if (ok)
		strcpy(tok_out, tok);
	return ok;
}

// Read the rest of a small body (up to max bytes) into buf.
static int read_small_body(struct req *r, char *buf, size_t max)
{
	if (r->content_length < 0 || (size_t)r->content_length >= max)
		return -1;
	size_t want = (size_t)r->content_length;
	size_t have = r->body_have < want ? r->body_have : want;
	memcpy(buf, r->body, have);
	while (have < (size_t)r->content_length) {
		ssize_t n = recv(r->fd, buf + have, r->content_length - have, 0);
		if (n <= 0)
			return -1;
		have += n;
	}
	buf[have] = 0;
	return 0;
}

// ------------------------------------------------------------------ static

static void serve_static(struct req *r)
{
	static const char *const assets[][2] = {
		{ "/", "index.html" }, { "/logo.png", "logo.png" }, { "/favicon.png", "favicon.png" },
	};
	const char *name = NULL;
	for (size_t i = 0; i < sizeof assets / sizeof assets[0]; i++)
		if (!strcmp(r->path, assets[i][0]))
			name = assets[i][1];
	if (!name) {
		respond_error(r->fd, 404, "not found");
		return;
	}
	char path[PATH_MAX];
	snprintf(path, sizeof path, "%s/%s", www_dir, name);
	int f = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (f < 0 || fstat(f, &st)) {
		respond_error(r->fd, 404, "missing index.html");
		if (f >= 0)
			close(f);
		return;
	}
	char h[512];
	int n = snprintf(h, sizeof h,
			 "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
			 "Content-Length: %lld\r\nCache-Control: no-cache\r\n"
			 "X-Frame-Options: DENY\r\nConnection: close\r\n\r\n",
			 strstr(name, ".png") ? "image/png" : "text/html; charset=utf-8", (long long)st.st_size);
	if (send_all(r->fd, h, n) == 0) {
		off_t off = 0;
		while (off < st.st_size)
			if (sendfile(r->fd, f, &off, st.st_size - off) <= 0)
				break;
	}
	close(f);
}

// --------------------------------------------------------------------- api

static void api_pair(struct req *r)
{
	char body[64];
	if (read_small_body(r, body, sizeof body)) {
		respond_error(r->fd, 400, "bad body");
		return;
	}
	char code[16];
	size_t n = 0;
	for (char *p = body; *p && n < sizeof code - 1; p++)
		if (isdigit((unsigned char)*p))
			code[n++] = *p;
	code[n] = 0;

	pthread_mutex_lock(&lock);
	int ok = n == 6 && !strcmp(code, pair_code);
	char tok[33] = "";
	if (ok) {
		random_hex(tok, 16);
		strcpy(sessions[next_session], tok);
		next_session = (next_session + 1) % MAX_SESSIONS;
		failures = 0;
	} else if (++failures >= MAX_FAILURES) {
		new_pair_code();
	}
	pthread_mutex_unlock(&lock);

	if (!ok) {
		sleep(1);	// slow down guessing
		respond_error(r->fd, 403, "wrong code");
		return;
	}
	char extra[160];
	snprintf(extra, sizeof extra,
		 "Set-Cookie: coz=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000\r\n", tok);
	respond(r->fd, 200, "application/json", extra, "{\"ok\":true}", 11);
	fprintf(stderr, "cozisboy-web: browser paired\n");
}

static void api_logout(struct req *r)
{
	pthread_mutex_lock(&lock);
	for (int s = 0; s < MAX_SESSIONS; s++)
		if (!strcmp(sessions[s], r->token))
			sessions[s][0] = 0;
	pthread_mutex_unlock(&lock);
	respond(r->fd, 200, "application/json",
		"Set-Cookie: coz=; Path=/; Max-Age=0\r\n", "{\"ok\":true}", 11);
}

static void wifi_info(FILE *f)
{
	char ssid[128] = "", line[256];
	FILE *p = popen("wpa_cli -p /run/wpa_supplicant -i wlan0 status 2>/dev/null", "r");
	if (p) {
		while (fgets(line, sizeof line, p))
			if (!strncmp(line, "ssid=", 5)) {
				snprintf(ssid, sizeof ssid, "%.127s", line + 5);
				ssid[strcspn(ssid, "\n")] = 0;
			}
		pclose(p);
	}
	int signal_dbm = 0;
	FILE *w = popen("iw dev wlan0 link 2>/dev/null", "r");
	if (w) {
		while (fgets(line, sizeof line, w))
			sscanf(line, " signal: %d dBm", &signal_dbm);
		pclose(w);
	}
	char ip[INET_ADDRSTRLEN] = "";
	struct ifaddrs *ifa, *i;
	if (getifaddrs(&ifa) == 0) {
		for (i = ifa; i; i = i->ifa_next)
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, "wlan0"))
				inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
		freeifaddrs(ifa);
	}
	fprintf(f, "\"wifi\":{\"ssid\":");
	json_str(f, ssid);
	fprintf(f, ",\"ip\":");
	json_str(f, ip);
	fprintf(f, ",\"signal_dbm\":%d}", signal_dbm);
}

static void api_status(struct req *r)
{
	char *buf;
	size_t len;
	FILE *f = open_memstream(&buf, &len);
	char s[64];

	fprintf(f, "{\"battery\":{\"capacity\":%ld,\"status\":", sysfs_long(BATTERY "capacity", -1));
	json_str(f, read_sysfs(BATTERY "status", s, sizeof s) ? "" : s);
	fprintf(f, ",\"voltage_mv\":%ld,\"current_ma\":%ld},",
		sysfs_long(BATTERY "voltage_now", 0) / 1000, sysfs_long(BATTERY "current_now", 0) / 1000);

	struct statvfs v;
	if (statvfs(ROOT, &v) == 0)
		fprintf(f, "\"storage\":{\"total\":%llu,\"free\":%llu},",
			(unsigned long long)v.f_blocks * v.f_frsize,
			(unsigned long long)v.f_bavail * v.f_frsize);

	wifi_info(f);

	double up = 0;
	FILE *u = fopen("/proc/uptime", "r");
	if (u) {
		if (fscanf(u, "%lf", &up) != 1)
			up = 0;
		fclose(u);
	}
	char host[64] = "";
	gethostname(host, sizeof host - 1);
	fprintf(f, ",\"uptime\":%.0f,\"hostname\":", up);
	json_str(f, host);
	fprintf(f, ",\"brightness\":%ld,\"max_brightness\":%ld}",
		sysfs_long(BACKLIGHT "brightness", 0), sysfs_long(BACKLIGHT "max_brightness", 0));
	fclose(f);
	respond(r->fd, 200, "application/json", NULL, buf, len);
	free(buf);
}

static void api_brightness(struct req *r)
{
	char v[16];
	if (query_param(r->query, "v", v, sizeof v)) {
		respond_error(r->fd, 400, "missing v");
		return;
	}
	long max = sysfs_long(BACKLIGHT "max_brightness", 255), val = atol(v);
	if (val < 1)	// the web UI never turns the screen fully off
		val = 1;
	if (val > max)
		val = max;
	int fd = open(BACKLIGHT "brightness", O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		respond_error(r->fd, 500, "no backlight");
		return;
	}
	int n = snprintf(v, sizeof v, "%ld", val);
	int ok = write(fd, v, n) == n;
	close(fd);
	if (!ok) {
		respond_error(r->fd, 500, "write failed");
		return;
	}
	respond(r->fd, 200, "application/json", NULL, "{\"ok\":true}", 11);
}

// ------------------------------------------------------------------ library
//
// Everything about games goes through common/library.h. The index (id -> system
// + name) is cached and rebuilt when the library changes: by us (invalidate)
// or by the launcher/SSH (a system folder's mtime moves).

static pthread_mutex_t lib_lock = PTHREAD_MUTEX_INITIALIZER;
static coz_lib cache;
static int cache_valid;
static time_t cache_mtimes[COZ_SYS_COUNT];

static void sys_mtimes(time_t *out)
{
	for (int s = 0; s < COZ_SYS_COUNT; s++) {
		char p[PATH_MAX];
		struct stat st;
		snprintf(p, sizeof p, "%s/%s", coz_lib_root(), coz_sys_key(s));
		out[s] = stat(p, &st) ? 0 : st.st_mtime;
	}
}

// Caller holds lib_lock.
static int cache_update(void)
{
	time_t m[COZ_SYS_COUNT];
	sys_mtimes(m);
	if (cache_valid && !memcmp(m, cache_mtimes, sizeof m))
		return 0;
	coz_lib_free(&cache);
	int r = coz_lib_scan(&cache);
	if (r)
		return r;
	memcpy(cache_mtimes, m, sizeof m);
	cache_valid = 1;
	return 0;
}

// A fresh copy of one game. Caller holds lib_lock.
static int lookup(const char *id, coz_game *out)
{
	int r = cache_update();
	if (r)
		return r;
	const coz_game *g = coz_lib_find(&cache, id);
	if (!g)
		return -ENOENT;
	*out = *g;
	return coz_game_refresh(out);
}

static int err_status(int r)
{
	switch (-r) {
	case EEXIST: return 409;
	case EINVAL: return 415;
	case ENOENT: return 404;
	case ENOSPC: return 507;
	case ENAMETOOLONG: return 400;
	default: return 500;
	}
}

static void respond_lib_error(int fd, int r, const char *what)
{
	char msg[160];
	switch (-r) {
	case EEXIST: snprintf(msg, sizeof msg, "%s already exists", what); break;
	case EINVAL: snprintf(msg, sizeof msg, "not a valid %s", what); break;
	case ENOENT: snprintf(msg, sizeof msg, "%s not found", what); break;
	case ENOSPC: snprintf(msg, sizeof msg, "not enough space"); break;
	default: snprintf(msg, sizeof msg, "%s: %s", what, strerror(-r)); break;
	}
	respond_error(fd, err_status(r), msg);
}

static void json_game(FILE *f, const coz_game *g)
{
	fprintf(f, "{\"id\":\"%s\",\"sys\":\"%s\",\"name\":", g->id, coz_sys_key(g->sys));
	json_str(f, g->name);
	fprintf(f, ",\"title\":");
	json_str(f, g->title);
	fprintf(f, ",\"region\":");
	json_str(f, g->region);
	fprintf(f, ",\"rom_size\":%lld,\"added\":%lld,\"save\":", g->rom_size, (long long)g->added);
	if (g->has_save)
		fprintf(f, "{\"size\":%lld,\"time\":%lld}", g->save_size, (long long)g->save_time);
	else
		fprintf(f, "null");
	fprintf(f, ",\"save_backup\":%s,\"cover\":%lld,\"states\":%d,\"last_state\":%lld}",
		g->has_save_backup ? "true" : "false", g->has_cover ? (long long)g->cover_time : 0LL,
		g->nstates, (long long)g->last_state);
}

static void respond_game(int fd, const coz_game *g)
{
	char *buf;
	size_t len;
	FILE *f = open_memstream(&buf, &len);
	json_game(f, g);
	fclose(f);
	respond(fd, 200, "application/json", NULL, buf, len);
	free(buf);
}

static void api_games_list(struct req *r)
{
	pthread_mutex_lock(&lib_lock);
	int rc = cache_update();
	if (rc) {
		pthread_mutex_unlock(&lib_lock);
		respond_lib_error(r->fd, rc, "library");
		return;
	}
	char *buf;
	size_t len;
	FILE *f = open_memstream(&buf, &len);
	fprintf(f, "{\"systems\":[");
	for (int s = 0; s < COZ_SYS_COUNT; s++) {
		fprintf(f, "%s{\"key\":\"%s\",\"short\":\"%s\",\"label\":", s ? "," : "",
			coz_sys_key(s), coz_sys_short(s));
		json_str(f, coz_sys_label(s));
		fputc('}', f);
	}
	fprintf(f, "],\"games\":[");
	for (int i = 0; i < cache.n; i++) {
		coz_game g = cache.games[i];
		coz_game_refresh(&g);	// saves/states change without touching the index
		if (i)
			fputc(',', f);
		json_game(f, &g);
	}
	fprintf(f, "]}");
	fclose(f);
	pthread_mutex_unlock(&lib_lock);
	respond(r->fd, 200, "application/json", NULL, buf, len);
	free(buf);
}

// Stream the request body into fd. 0 ok, -1 write error, -2 connection lost.
static int recv_body(struct req *r, int fd)
{
	long long left = r->content_length;
	size_t have = r->body_have < (size_t)left ? r->body_have : (size_t)left;
	if (have && write(fd, r->body, have) != (ssize_t)have)
		return -1;
	left -= have;
	enum { CHUNK = 256 * 1024 };
	char *chunk = malloc(CHUNK);
	int err = chunk ? 0 : -1;
	while (!err && left > 0) {
		ssize_t n = recv(r->fd, chunk, left < CHUNK ? left : CHUNK, 0);
		if (n <= 0)
			err = -2;
		else if (write(fd, chunk, n) != n)
			err = -1;
		else
			left -= n;
	}
	free(chunk);
	return err;
}

static int check_upload(struct req *r)
{
	if (r->content_length < 0) {
		respond_error(r->fd, 411, "length required");
		return -1;
	}
	struct statvfs v;
	if (statvfs(coz_lib_root(), &v) == 0 &&
	    (unsigned long long)r->content_length + (8 << 20) > (unsigned long long)v.f_bavail * v.f_frsize) {
		respond_error(r->fd, 507, "not enough space");
		return -1;
	}
	return 0;
}

static void api_game_add(struct req *r)
{
	char filename[COZ_NAME_MAX + 16], rep[4] = "0";
	if (query_param(r->query, "filename", filename, sizeof filename)) {
		respond_error(r->fd, 400, "missing filename");
		return;
	}
	query_param(r->query, "replace", rep, sizeof rep);
	if (check_upload(r))
		return;
	char root[PATH_MAX];
	snprintf(root, sizeof root, "%s", coz_lib_root());
	mkdir(root, 0755);
	coz_import imp;
	int rc = coz_import_begin(&imp, filename);
	if (rc) {
		respond_lib_error(r->fd, rc, rc == -EINVAL ? "game file (.gb, .gbc or .gba)" : "game");
		return;
	}
	int e = recv_body(r, imp.fd);
	if (e) {
		coz_import_abort(&imp);
		if (e == -1)
			respond_error(r->fd, 500, "write failed");
		return;
	}
	coz_game g;
	pthread_mutex_lock(&lib_lock);
	rc = coz_import_commit(&imp, rep[0] == '1', &g);
	cache_valid = 0;
	pthread_mutex_unlock(&lib_lock);
	if (rc) {
		respond_lib_error(r->fd, rc, rc == -EINVAL ? "game: the file isn't a ROM for this system" : "game");
		return;
	}
	respond_game(r->fd, &g);
}

static void send_file(struct req *r, const char *path, const char *ctype,
		      const char *download_name, int cacheable)
{
	int f = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (f < 0 || fstat(f, &st) || !S_ISREG(st.st_mode)) {
		if (f >= 0)
			close(f);
		respond_error(r->fd, 404, "not found");
		return;
	}
	char disp[COZ_NAME_MAX * 3 + 96] = "";
	if (download_name) {
		size_t o = snprintf(disp, sizeof disp, "Content-Disposition: attachment; filename*=UTF-8''");
		for (const unsigned char *p = (const unsigned char *)download_name; *p && o < sizeof disp - 8; p++)
			o += isalnum(*p) || strchr("-._~", *p) ? (size_t)sprintf(disp + o, "%c", *p)
								: (size_t)sprintf(disp + o, "%%%02X", *p);
		snprintf(disp + o, sizeof disp - o, "\r\n");
	}
	char h[1024];
	int n = snprintf(h, sizeof h,
			 "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n%s"
			 "Cache-Control: %s\r\nConnection: close\r\n\r\n",
			 ctype, (long long)st.st_size, disp,
			 cacheable ? "private, max-age=31536000, immutable" : "no-store");
	if (send_all(r->fd, h, n) == 0) {
		off_t off = 0;
		while (off < st.st_size)
			if (sendfile(r->fd, f, &off, st.st_size - off) <= 0)
				break;
	}
	close(f);
}

// Import a save or cover from the request body.
static void api_game_put_file(struct req *r, coz_game *g, int is_cover)
{
	if (check_upload(r))
		return;
	if (is_cover && r->content_length > (4 << 20)) {
		respond_error(r->fd, 413, "cover too large");
		return;
	}
	if (!is_cover && r->content_length > (4 << 20)) {
		respond_error(r->fd, 413, "save too large");	// GB/GBA saves are at most 128 KB
		return;
	}
	coz_import imp;
	int rc = is_cover ? coz_cover_import_begin(g, &imp) : coz_save_import_begin(g, &imp);
	if (rc) {
		respond_lib_error(r->fd, rc, is_cover ? "cover" : "save");
		return;
	}
	int e = recv_body(r, imp.fd);
	if (e) {
		coz_import_abort(&imp);
		if (e == -1)
			respond_error(r->fd, 500, "write failed");
		return;
	}
	pthread_mutex_lock(&lib_lock);
	rc = is_cover ? coz_cover_import_commit(g, &imp) : coz_save_import_commit(g, &imp);
	if (!rc)
		coz_game_refresh(g);
	pthread_mutex_unlock(&lib_lock);
	if (rc)
		respond_lib_error(r->fd, rc, is_cover ? "cover (PNG)" : "save");
	else
		respond_game(r->fd, g);
}

static void api_states_list(struct req *r, const coz_game *g)
{
	coz_state *s;
	int n;
	pthread_mutex_lock(&lib_lock);
	int rc = coz_states_list(g, &s, &n);
	pthread_mutex_unlock(&lib_lock);
	if (rc) {
		respond_lib_error(r->fd, rc, "states");
		return;
	}
	char *buf;
	size_t len;
	FILE *f = open_memstream(&buf, &len);
	fprintf(f, "{\"states\":[");
	for (int i = 0; i < n; i++)
		fprintf(f, "%s{\"id\":\"%s\",\"time\":%lld,\"size\":%lld,\"thumb\":%s}", i ? "," : "",
			s[i].id, (long long)s[i].time, s[i].size, s[i].has_thumb ? "true" : "false");
	fprintf(f, "]}");
	fclose(f);
	free(s);
	respond(r->fd, 200, "application/json", NULL, buf, len);
	free(buf);
}

// ---- zip backup (stored, no compression: saves are small, PNGs compressed)

struct zent {
	char path[PATH_MAX];
	char name[COZ_NAME_MAX * 2 + 64];
	uint32_t size, crc, dostime;
	uint32_t offset;
};

static uint32_t crc32_file(const char *path, uint32_t *size)
{
	static uint32_t t[256];
	if (!t[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;
			for (int k = 0; k < 8; k++)
				v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
			t[i] = v;
		}
	uint32_t c = 0xffffffffu;
	*size = 0;
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	unsigned char b[65536];
	size_t n;
	while ((n = fread(b, 1, sizeof b, f)) > 0) {
		for (size_t i = 0; i < n; i++)
			c = t[(c ^ b[i]) & 0xff] ^ (c >> 8);
		*size += n;
	}
	fclose(f);
	return c ^ 0xffffffffu;
}

static uint32_t dos_time(time_t t)
{
	struct tm tm;
	localtime_r(&t, &tm);
	if (tm.tm_year < 80)
		return (1 << 21) | (1 << 16);	// 1980-01-01
	return (uint32_t)(tm.tm_year - 80) << 25 | (uint32_t)(tm.tm_mon + 1) << 21 |
	       (uint32_t)tm.tm_mday << 16 | (uint32_t)tm.tm_hour << 11 | (uint32_t)tm.tm_min << 5 |
	       (uint32_t)tm.tm_sec / 2;
}

static void le16(unsigned char *p, unsigned v) { p[0] = v, p[1] = v >> 8; }
static void le32(unsigned char *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }

static int zent_add(struct zent **z, int *n, int *cap, const char *path, const char *name)
{
	struct stat st;
	if (stat(path, &st) || !S_ISREG(st.st_mode))
		return 0;
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 64;
		struct zent *nz = realloc(*z, *cap * sizeof **z);
		if (!nz)
			return -1;
		*z = nz;
	}
	struct zent *e = &(*z)[(*n)++];
	snprintf(e->path, sizeof e->path, "%s", path);
	snprintf(e->name, sizeof e->name, "%s", name);
	e->crc = crc32_file(path, &e->size);
	e->dostime = dos_time(st.st_mtime);
	return 0;
}

// GET /api/backup[?states=1]: every battery save (and state), as a zip:
//   saves/<System>/<Game>.sav, states/<System>/<Game>/<time>.state|.png
static void api_backup(struct req *r)
{
	char st[4] = "0";
	query_param(r->query, "states", st, sizeof st);
	struct zent *z = NULL;
	int n = 0, cap = 0;

	pthread_mutex_lock(&lib_lock);
	cache_update();
	for (int i = 0; i < cache.n; i++) {
		coz_game g = cache.games[i];
		coz_game_refresh(&g);
		char p[PATH_MAX], name[sizeof z->name];
		if (g.has_save) {
			coz_game_save_path(&g, p, sizeof p);
			snprintf(name, sizeof name, "saves/%s/%s.sav", coz_sys_label(g.sys), g.name);
			zent_add(&z, &n, &cap, p, name);
		}
		if (st[0] == '1' && g.nstates) {
			coz_state *s;
			int ns;
			if (coz_states_list(&g, &s, &ns))
				continue;
			for (int k = 0; k < ns; k++) {
				coz_state_path(&g, s[k].id, p, sizeof p);
				snprintf(name, sizeof name, "states/%s/%s/%s.state", coz_sys_label(g.sys), g.name, s[k].id);
				zent_add(&z, &n, &cap, p, name);
				if (s[k].has_thumb) {
					coz_state_thumb_path(&g, s[k].id, p, sizeof p);
					snprintf(name, sizeof name, "states/%s/%s/%s.png", coz_sys_label(g.sys), g.name, s[k].id);
					zent_add(&z, &n, &cap, p, name);
				}
			}
			free(s);
		}
	}
	pthread_mutex_unlock(&lib_lock);

	// Exact length up front: local headers + data + central directory + end.
	uint64_t off = 0, central = 0;
	for (int i = 0; i < n; i++) {
		z[i].offset = off;
		off += 30 + strlen(z[i].name) + z[i].size;
		central += 46 + strlen(z[i].name);
	}
	if (off + central > 0xffffffffull) {
		free(z);
		respond_error(r->fd, 413, "backup too large for zip32");
		return;
	}
	uint64_t total = off + central + 22;
	char date[32], h[512];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(date, sizeof date, "%Y-%m-%d", &tm);
	int hn = snprintf(h, sizeof h,
			  "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: %llu\r\n"
			  "Content-Disposition: attachment; filename=\"cozisboy-%s-%s.zip\"\r\n"
			  "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
			  (unsigned long long)total, st[0] == '1' ? "saves-and-states" : "saves", date);
	if (send_all(r->fd, h, hn))
		goto out;
	for (int i = 0; i < n; i++) {
		unsigned char l[30] = { 0 };
		size_t nl = strlen(z[i].name);
		le32(l, 0x04034b50);
		le16(l + 4, 20);
		le16(l + 6, 1 << 11);	// UTF-8 names
		le32(l + 10, z[i].dostime);
		le32(l + 14, z[i].crc);
		le32(l + 18, z[i].size);
		le32(l + 22, z[i].size);
		le16(l + 26, nl);
		if (send_all(r->fd, l, 30) || send_all(r->fd, z[i].name, nl))
			goto out;
		int f = open(z[i].path, O_RDONLY | O_CLOEXEC);
		if (f < 0)
			goto out;	// vanished meanwhile: the zip would be short, abort
		off_t o = 0;
		while (o < (off_t)z[i].size)
			if (sendfile(r->fd, f, &o, z[i].size - o) <= 0)
				break;
		close(f);
		if (o != (off_t)z[i].size)
			goto out;
	}
	for (int i = 0; i < n; i++) {
		unsigned char c[46] = { 0 };
		size_t nl = strlen(z[i].name);
		le32(c, 0x02014b50);
		le16(c + 4, 20);
		le16(c + 6, 20);
		le16(c + 8, 1 << 11);
		le32(c + 12, z[i].dostime);
		le32(c + 16, z[i].crc);
		le32(c + 20, z[i].size);
		le32(c + 24, z[i].size);
		le16(c + 28, nl);
		le32(c + 42, z[i].offset);
		if (send_all(r->fd, c, 46) || send_all(r->fd, z[i].name, nl))
			goto out;
	}
	unsigned char e[22] = { 0 };
	le32(e, 0x06054b50);
	le16(e + 8, n);
	le16(e + 10, n);
	le32(e + 12, central);
	le32(e + 16, off);
	send_all(r->fd, e, 22);
out:
	free(z);
}

// /api/games/<id>[/<a>[/<b>[/<c>]]]
static void api_game(struct req *r, const char *rest)
{
	char id[COZ_ID_LEN], a[16] = "", b[32] = "", c[16] = "";
	char seg[4][64] = { "" };
	int ns = 0;
	for (const char *p = rest; *p && ns < 4;) {
		const char *e = strchrnul(p, '/');
		size_t l = e - p;
		if (l == 0 || l >= sizeof seg[0]) {
			respond_error(r->fd, 404, "bad path");
			return;
		}
		memcpy(seg[ns], p, l);
		seg[ns++][l] = 0;
		p = *e ? e + 1 : e;
	}
	if (ns < 1 || strlen(seg[0]) != 16) {
		respond_error(r->fd, 404, "no such game");
		return;
	}
	snprintf(id, sizeof id, "%s", seg[0]);
	snprintf(a, sizeof a, "%.15s", seg[1]);
	snprintf(b, sizeof b, "%.31s", seg[2]);
	snprintf(c, sizeof c, "%.15s", seg[3]);

	coz_game g;
	pthread_mutex_lock(&lib_lock);
	int rc = lookup(id, &g);
	pthread_mutex_unlock(&lib_lock);
	if (rc) {
		respond_lib_error(r->fd, rc, "game");
		return;
	}
	int get = !strcmp(r->method, "GET"), put = !strcmp(r->method, "PUT");
	int post = !strcmp(r->method, "POST"), del = !strcmp(r->method, "DELETE");
	char p[PATH_MAX], dl[COZ_NAME_MAX + 32];

	if (ns == 1 && get) {
		respond_game(r->fd, &g);
	} else if (ns == 1 && del) {
		pthread_mutex_lock(&lib_lock);
		rc = coz_game_delete(&g);
		cache_valid = 0;
		pthread_mutex_unlock(&lib_lock);
		if (rc)
			respond_lib_error(r->fd, rc, "game");
		else
			respond(r->fd, 200, "application/json", NULL, "{\"ok\":true}", 11);
	} else if (ns == 2 && post && !strcmp(a, "rename")) {
		char name[COZ_NAME_MAX + 1];
		if (query_param(r->query, "name", name, sizeof name)) {
			respond_error(r->fd, 400, "missing name");
			return;
		}
		pthread_mutex_lock(&lib_lock);
		rc = coz_game_rename(&g, name);
		cache_valid = 0;
		pthread_mutex_unlock(&lib_lock);
		if (rc)
			respond_lib_error(r->fd, rc, rc == -EINVAL ? "name" : "game with that name");
		else
			respond_game(r->fd, &g);
	} else if (ns == 2 && get && !strcmp(a, "rom")) {
		coz_game_rom_path(&g, p, sizeof p);
		snprintf(dl, sizeof dl, "%s.%s", g.name, coz_sys_key(g.sys));
		send_file(r, p, "application/octet-stream", dl, 0);
	} else if (ns == 2 && !strcmp(a, "cover")) {
		if (get) {
			coz_game_cover_path(&g, p, sizeof p);
			send_file(r, p, "image/png", NULL, 1);	// URL carries a version
		} else if (put) {
			api_game_put_file(r, &g, 1);
		} else if (del) {
			rc = coz_cover_delete(&g);
			if (rc)
				respond_lib_error(r->fd, rc, "cover");
			else
				respond(r->fd, 200, "application/json", NULL, "{\"ok\":true}", 11);
		} else
			respond_error(r->fd, 405, "method not allowed");
	} else if (!strcmp(a, "save")) {
		if (ns == 2 && get) {
			coz_game_save_path(&g, p, sizeof p);
			snprintf(dl, sizeof dl, "%s.sav", g.name);
			send_file(r, p, "application/octet-stream", dl, 0);
		} else if (ns == 2 && put) {
			api_game_put_file(r, &g, 0);
		} else if (ns == 2 && del) {
			pthread_mutex_lock(&lib_lock);
			rc = coz_save_delete(&g);
			coz_game_refresh(&g);
			pthread_mutex_unlock(&lib_lock);
			if (rc)
				respond_lib_error(r->fd, rc, "save");
			else
				respond_game(r->fd, &g);
		} else if (ns == 3 && get && !strcmp(b, "backup")) {
			coz_game_save_backup_path(&g, p, sizeof p);
			snprintf(dl, sizeof dl, "%s (previous).sav", g.name);
			send_file(r, p, "application/octet-stream", dl, 0);
		} else if (ns == 3 && post && !strcmp(b, "restore")) {
			pthread_mutex_lock(&lib_lock);
			rc = coz_save_restore_backup(&g);
			coz_game_refresh(&g);
			pthread_mutex_unlock(&lib_lock);
			if (rc)
				respond_lib_error(r->fd, rc, "previous save");
			else
				respond_game(r->fd, &g);
		} else
			respond_error(r->fd, 404, "no such endpoint");
	} else if (!strcmp(a, "states")) {
		if (ns == 2 && get) {
			api_states_list(r, &g);
		} else if (ns == 3 && get) {
			if (coz_state_path(&g, b, p, sizeof p)) {
				respond_error(r->fd, 404, "no such state");
				return;
			}
			snprintf(dl, sizeof dl, "%s %s.state", g.name, b);
			send_file(r, p, "application/octet-stream", dl, 0);
		} else if (ns == 4 && get && !strcmp(c, "thumb")) {
			if (coz_state_thumb_path(&g, b, p, sizeof p)) {
				respond_error(r->fd, 404, "no such state");
				return;
			}
			send_file(r, p, "image/png", NULL, 1);
		} else if (ns == 3 && del) {
			pthread_mutex_lock(&lib_lock);
			rc = coz_state_delete(&g, b);
			pthread_mutex_unlock(&lib_lock);
			if (rc)
				respond_lib_error(r->fd, rc, "state");
			else
				respond(r->fd, 200, "application/json", NULL, "{\"ok\":true}", 11);
		} else
			respond_error(r->fd, 404, "no such endpoint");
	} else {
		respond_error(r->fd, 404, "no such endpoint");
	}
}

// Current framebuffer contents as a 24-bit BMP (browsers display BMP natively).
static void api_screenshot(struct req *r)
{
	int fb = open("/dev/fb0", O_RDONLY | O_CLOEXEC);
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix) ||
	    var.bits_per_pixel != 32) {
		if (fb >= 0)
			close(fb);
		respond_error(r->fd, 500, "no framebuffer");
		return;
	}
	unsigned w = var.xres, h = var.yres, stride = fix.line_length;
	size_t fbsize = (size_t)stride * h;
	unsigned char *src = malloc(fbsize);
	size_t rowb = (w * 3 + 3) & ~3u, imgsize = rowb * h;
	unsigned char *bmp = malloc(54 + imgsize);
	if (!src || !bmp ||
	    pread(fb, src, fbsize, (off_t)var.yoffset * stride) != (ssize_t)fbsize) {
		close(fb);
		free(src);
		free(bmp);
		respond_error(r->fd, 500, "read failed");
		return;
	}
	close(fb);
	memset(bmp, 0, 54);
	uint32_t hdr[] = { 54 + imgsize, 0, 54, 40, w, h };
	bmp[0] = 'B';
	bmp[1] = 'M';
	memcpy(bmp + 2, &hdr[0], 4);
	memcpy(bmp + 10, &hdr[2], 4);
	memcpy(bmp + 14, &hdr[3], 4);
	memcpy(bmp + 18, &hdr[4], 4);
	memcpy(bmp + 22, &hdr[5], 4);
	bmp[26] = 1;
	bmp[28] = 24;
	memcpy(bmp + 34, &imgsize, 4);
	for (unsigned y = 0; y < h; y++) {
		const unsigned char *s = src + (size_t)(h - 1 - y) * stride;	// bottom-up
		unsigned char *d = bmp + 54 + rowb * y;
		for (unsigned x = 0; x < w; x++) {	// XRGB8888 LE = B G R X
			d[3 * x] = s[4 * x];
			d[3 * x + 1] = s[4 * x + 1];
			d[3 * x + 2] = s[4 * x + 2];
		}
	}
	free(src);
	respond(r->fd, 200, "image/bmp", NULL, bmp, 54 + imgsize);
	free(bmp);
}

// ------------------------------------------------------------------ router

static void route(struct req *r)
{
	int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST");
	int put = !strcmp(r->method, "PUT");

	if (strncmp(r->path, "/api/", 5)) {
		if (get)
			serve_static(r);
		else
			respond_error(r->fd, 405, "method not allowed");
		return;
	}
	const char *ep = r->path + 5;
	// Every API call must come from our page's script (custom header):
	// a cross-site form or image can't add it.
	if (!r->has_xhdr) {
		respond_error(r->fd, 403, "missing X-Cozis header");
		return;
	}
	if (post && !strcmp(ep, "pair")) {
		api_pair(r);
		return;
	}
	if (!r->authed) {
		respond_error(r->fd, 401, "not paired");
		return;
	}
	if (get && !strcmp(ep, "status"))
		api_status(r);
	else if (!strcmp(ep, "games") && get)
		api_games_list(r);
	else if (!strcmp(ep, "games") && put)
		api_game_add(r);
	else if (!strncmp(ep, "games/", 6))
		api_game(r, ep + 6);
	else if (get && !strcmp(ep, "backup"))
		api_backup(r);
	else if (post && !strcmp(ep, "brightness"))
		api_brightness(r);
	else if (get && !strcmp(ep, "screenshot"))
		api_screenshot(r);
	else if (post && !strcmp(ep, "logout"))
		api_logout(r);
	else
		respond_error(r->fd, 404, "no such endpoint");
}

static void *handle(void *arg)
{
	int fd = (int)(intptr_t)arg;
	struct timeval tv = { .tv_sec = 60 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	char *hbuf = malloc(HDR_MAX + 1);
	size_t got = 0;
	char *end = NULL;
	while (got < HDR_MAX) {
		ssize_t n = recv(fd, hbuf + got, HDR_MAX - got, 0);
		if (n <= 0)
			goto out;
		got += n;
		hbuf[got] = 0;
		if ((end = strstr(hbuf, "\r\n\r\n")))
			break;
	}
	if (!end) {
		respond_error(fd, 413, "headers too large");
		goto out;
	}
	*end = 0;

	struct req r = { .fd = fd, .content_length = -1 };
	r.body = end + 4;
	r.body_have = got - (r.body - hbuf);

	char target[3072];
	if (sscanf(hbuf, "%7s %3071s", r.method, target) != 2) {
		respond_error(fd, 400, "bad request line");
		goto out;
	}
	char *q = strchr(target, '?');
	if (q) {
		*q++ = 0;
		snprintf(r.query, sizeof r.query, "%s", q);
	}
	snprintf(r.path, sizeof r.path, "%.1023s", target);

	for (char *line = strstr(hbuf, "\r\n"); line; line = strstr(line, "\r\n")) {
		line += 2;
		if (!strncasecmp(line, "Content-Length:", 15))
			r.content_length = atoll(line + 15);
		else if (!strncasecmp(line, "X-Cozis:", 8))
			r.has_xhdr = 1;
		else if (!strncasecmp(line, "Cookie:", 7)) {
			char *eol = strstr(line, "\r\n");
			char save = 0;
			if (eol) {
				save = *eol;
				*eol = 0;
			}
			r.authed = check_session(line + 7, r.token);
			if (eol)
				*eol = save;
		}
	}
	// Download links and <img> can't send headers: for these read-only GETs
	// ?x=1 stands in for X-Cozis (the session cookie is still required).
	if (dev_noauth)	// PC development only (COZ_WEB_INSECURE_DEV=1)
		r.authed = 1;
	char x[4];
	if (!strcmp(r.method, "GET") && query_param(r.query, "x", x, sizeof x) == 0 &&
	    (!strncmp(r.path, "/api/games/", 11) || !strcmp(r.path, "/api/backup") ||
	     !strcmp(r.path, "/api/screenshot")))
		r.has_xhdr = 1;

	route(&r);
out:
	free(hbuf);
	close(fd);
	return NULL;
}

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 80;
	const char *w = getenv("COZ_WWW");
	if (w)
		www_dir = w;
	signal(SIGPIPE, SIG_IGN);
	dev_noauth = getenv("COZ_WEB_INSECURE_DEV") && !strcmp(getenv("COZ_WEB_INSECURE_DEV"), "1");
	if (dev_noauth)
		fprintf(stderr, "cozisboy-web: WARNING: pairing disabled (COZ_WEB_INSECURE_DEV=1)\n");
	umask(022);

	pthread_mutex_lock(&lock);
	new_pair_code();
	pthread_mutex_unlock(&lock);

	int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 16)) {
		perror("bind/listen");
		return 1;
	}
	fprintf(stderr, "cozisboy-web: listening on port %d\n", port);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 128 * 1024);
	for (;;) {
		int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
		if (c < 0) {
			if (errno == EINTR || errno == ECONNABORTED || errno == EMFILE)
				continue;
			perror("accept");
			continue;
		}
		pthread_t t;
		if (pthread_create(&t, &attr, handle, (void *)(intptr_t)c))
			close(c);
	}
}
