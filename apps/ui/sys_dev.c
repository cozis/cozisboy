// System services on the device: Wi-Fi through wpa_supplicant (S45wifi, networks
// in /data/wifi/wpa_supplicant.conf) and the dropbear SSH server.
//
// Wi-Fi passwords never reach a command line: wpa_passphrase reads them on a
// pipe and only the derived key (psk=<hex>) is written to the config file.
#include "sys.h"
float ui_time(void); // ui.c (not ui_priv.h: its W/H macros)

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define WIFI_CONF "/data/wifi/wpa_supplicant.conf"
#define WIFI_OFF_FLAG "/data/wifi/off"
#define WIFI_INIT "/etc/init.d/S45wifi"
#define WPA_CLI "/usr/sbin/wpa_cli", "-p", "/run/wpa_supplicant", "-i", "wlan0"
#define SSH_OFF_FLAG "/data/ssh-off"
#define SSH_PW_FLAG "/data/ssh-password-login"
#define SSH_INIT "/etc/init.d/S50dropbear"
#define CONNECT_TIMEOUT 25.0f

/* ---- running helpers ---------------------------------------------------------- */

// Run argv, feed `in` on stdin, collect stdout into out. Returns the exit code.
static int run(char *const argv[], const char *in, char *out, size_t outlen)
{
	// Without `out`, stdout goes to /dev/null: a daemon started by a service
	// script would otherwise keep the pipe open and we'd wait forever.
	int pin[2], pout[2] = {-1, -1};
	if (pipe(pin) || (out && pipe(pout)))
		return -1;
	pid_t p = fork();
	if (p < 0)
		return -1;
	if (p == 0) {
		dup2(pin[0], 0);
		int nul = open("/dev/null", O_WRONLY);
		dup2(out ? pout[1] : nul, 1);
		if (nul >= 0)
			dup2(nul, 2);
		close(pin[1]);
		if (out)
			close(pout[0]);
		execv(argv[0], argv);
		_exit(127);
	}
	close(pin[0]);
	if (out)
		close(pout[1]);
	if (in) {
		ssize_t w = write(pin[1], in, strlen(in));
		(void)w;
	}
	close(pin[1]);
	if (out) {
		size_t got = 0;
		ssize_t n;
		char sink[256];
		while ((n = read(pout[0], got < outlen - 1 ? out + got : sink,
				 got < outlen - 1 ? outlen - 1 - got : sizeof sink)) > 0)
			if (got < outlen - 1)
				got += n;
		close(pout[0]);
		out[got] = 0;
	}
	int st;
	waitpid(p, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int sh(const char *script, const char *arg)
{
	char *const argv[] = {(char *)"/bin/sh", (char *)script, (char *)arg, NULL};
	return run(argv, NULL, NULL, 0);
}

static int pidfile_alive(const char *path)
{
	FILE *f = fopen(path, "r");
	int pid = 0;
	if (f) {
		if (fscanf(f, "%d", &pid) != 1)
			pid = 0;
		fclose(f);
	}
	return pid > 0 && kill(pid, 0) == 0;
}

static void touch(const char *path, int on)
{
	if (on) {
		int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (fd >= 0)
			close(fd);
	} else {
		unlink(path);
	}
	sync();
}

/* ---- Wi-Fi config file -------------------------------------------------------- */

static void conf_ensure(void)
{
	mkdir("/data/wifi", 0700);
	if (access(WIFI_CONF, F_OK) == 0)
		return;
	FILE *f = fopen(WIFI_CONF, "w");
	if (f) {
		fprintf(f, "ctrl_interface=/run/wpa_supplicant\nupdate_config=1\n\n");
		fclose(f);
		chmod(WIFI_CONF, 0600);
	}
}

// Rewrite the config without the network block for ssid; optionally append one.
// Returns the highest priority seen.
static int conf_replace(const char *ssid, const char *add_block)
{
	FILE *in = fopen(WIFI_CONF, "r");
	FILE *out = fopen(WIFI_CONF ".new", "w");
	if (!out) {
		if (in)
			fclose(in);
		return 0;
	}
	char line[512], block[4096];
	char want[80];
	snprintf(want, sizeof want, "ssid=\"%s\"", ssid);
	int inblk = 0, drop = 0, maxprio = 0;
	size_t bl = 0;
	while (in && fgets(line, sizeof line, in)) {
		const char *t = line + strspn(line, " \t");
		if (!inblk && !strncmp(t, "network={", 9)) {
			inblk = 1, drop = 0, bl = 0;
		}
		if (inblk) {
			size_t l = strlen(line);
			if (bl + l < sizeof block) {
				memcpy(block + bl, line, l);
				bl += l;
			}
			char tl[512];
			snprintf(tl, sizeof tl, "%s", t);
			tl[strcspn(tl, "\n")] = 0;
			if (!strcmp(tl, want))
				drop = 1;
			int pr;
			if (sscanf(t, "priority=%d", &pr) == 1 && pr > maxprio)
				maxprio = pr;
			if (t[0] == '}') {
				inblk = 0;
				if (!drop)
					fwrite(block, 1, bl, out);
			}
			continue;
		}
		fputs(line, out);
	}
	if (in)
		fclose(in);
	if (add_block)
		fputs(add_block, out);
	fflush(out);
	fsync(fileno(out));
	fclose(out);
	chmod(WIFI_CONF ".new", 0600);
	rename(WIFI_CONF ".new", WIFI_CONF);
	return maxprio;
}

static int conf_has(const char *ssid)
{
	FILE *f = fopen(WIFI_CONF, "r");
	if (!f)
		return 0;
	char line[512], want[80];
	snprintf(want, sizeof want, "ssid=\"%s\"", ssid);
	int found = 0;
	while (!found && fgets(line, sizeof line, f)) {
		const char *t = line + strspn(line, " \t");
		found = !strncmp(t, want, strlen(want)) && (t[strlen(want)] == '\n' || !t[strlen(want)]);
	}
	fclose(f);
	return found;
}

/* ---- Wi-Fi state -------------------------------------------------------------- */

static struct {
	int state;
	char ssid[33], ip[INET_ADDRSTRLEN];
	char want[33];		// network being joined
	float connect_t, poll_t, scan_t;
	int saw_handshake;
	const char *err;
	net_t nets[32];
	int nnets;
} W = {.state = WIFI_DISCONNECTED};

static int wifi_running(void)
{
	return pidfile_alive("/run/wpa_supplicant.pid");
}

static void wpa(const char *cmd, const char *arg, char *out, size_t outlen)
{
	char *const argv[] = {WPA_CLI, (char *)cmd, (char *)arg, NULL};
	if (!arg) {
		char *const a2[] = {WPA_CLI, (char *)cmd, NULL};
		run(a2, NULL, out, outlen);
		return;
	}
	run(argv, NULL, out, outlen);
}

static void poll_status(void)
{
	if (!wifi_running()) {
		W.state = access(WIFI_OFF_FLAG, F_OK) == 0 ? WIFI_OFF : WIFI_DISCONNECTED;
		W.ssid[0] = W.ip[0] = 0;
		return;
	}
	char out[1024], st[32] = "";
	wpa("status", NULL, out, sizeof out);
	W.ssid[0] = W.ip[0] = 0;
	for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {
		if (!strncmp(l, "wpa_state=", 10))
			snprintf(st, sizeof st, "%s", l + 10);
		else if (!strncmp(l, "ssid=", 5))
			snprintf(W.ssid, sizeof W.ssid, "%s", l + 5);
		else if (!strncmp(l, "ip_address=", 11))
			snprintf(W.ip, sizeof W.ip, "%s", l + 11);
	}
	if (!strcmp(st, "4WAY_HANDSHAKE") || !strcmp(st, "GROUP_HANDSHAKE"))
		W.saw_handshake = 1;
	int up = !strcmp(st, "COMPLETED") && W.ip[0];
	if (W.want[0]) {	// joining a network
		if (up && !strcmp(W.ssid, W.want)) {
			W.state = WIFI_CONNECTED;
			W.want[0] = 0;
		} else if (W.connect_t > CONNECT_TIMEOUT) {
			W.state = WIFI_FAILED;
			W.err = W.saw_handshake ? "WRONG PASSWORD" : "COULDN'T CONNECT";
			W.want[0] = 0;
		} else {
			W.state = WIFI_CONNECTING;
		}
		return;
	}
	if (W.state == WIFI_FAILED)
		return;	// keep showing the error until the next action
	W.state = up ? WIFI_CONNECTED
		: (!strcmp(st, "COMPLETED") || strstr(st, "ASSOC") || strstr(st, "HANDSHAKE") ||
		   !strcmp(st, "AUTHENTICATING")) ? WIFI_CONNECTING : WIFI_DISCONNECTED;
}

static int bars(int dbm)
{
	return dbm >= -55 ? 4 : dbm >= -67 ? 3 : dbm >= -75 ? 2 : 1;
}

static void read_scan(void)
{
	char out[16384];
	wpa("scan_results", NULL, out, sizeof out);
	int sig[32];
	W.nnets = 0;
	char *save;
	for (char *l = strtok_r(out, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char bssid[32], flags[256], ssid[256];
		int freq, dbm;
		if (sscanf(l, "%31s\t%d\t%d\t%255[^\t]\t%255[^\n]", bssid, &freq, &dbm, flags, ssid) != 5)
			continue;	// header, or hidden network (no ssid)
		if (strlen(ssid) > 32 || strstr(ssid, "\\x"))
			continue;
		int k;
		for (k = 0; k < W.nnets; k++)
			if (!strcmp(W.nets[k].ssid, ssid))
				break;
		if (k < W.nnets) {
			if (dbm > sig[k])
				sig[k] = dbm, W.nets[k].bars = bars(dbm);
			continue;
		}
		if (W.nnets == 32)
			continue;
		net_t *n = &W.nets[W.nnets];
		snprintf(n->ssid, sizeof n->ssid, "%s", ssid);
		n->bars = bars(dbm);
		n->secure = strstr(flags, "WPA") || strstr(flags, "WEP") || strstr(flags, "SAE") || strstr(flags, "RSN");
		n->saved = conf_has(ssid);
		sig[W.nnets++] = dbm;
	}
	// strongest first
	for (int i = 1; i < W.nnets; i++)
		for (int j = i; j > 0 && sig[j] > sig[j - 1]; j--) {
			net_t t = W.nets[j];
			W.nets[j] = W.nets[j - 1], W.nets[j - 1] = t;
			int s = sig[j];
			sig[j] = sig[j - 1], sig[j - 1] = s;
		}
}

void sys_tick(float dt)
{
	if (W.want[0])
		W.connect_t += dt;
	if (W.scan_t > 0) {
		W.scan_t -= dt;
		if (W.scan_t <= 0 && wifi_running())
			read_scan();
	}
	W.poll_t -= dt;
	if (W.poll_t <= 0) {
		W.poll_t = W.want[0] ? 0.5f : 1.5f;
		poll_status();
	}
}

int sys_wifi_enabled(void)
{
	return access(WIFI_OFF_FLAG, F_OK) != 0;
}

void sys_wifi_enable(int on)
{
	touch(WIFI_OFF_FLAG, !on);
	if (on) {
		conf_ensure();
		sh(WIFI_INIT, "start");
		sys_wifi_rescan();
	} else {
		sh(WIFI_INIT, "stop");
		char *const argv[] = {(char *)"/sbin/ip", (char *)"link", (char *)"set", (char *)"wlan0", (char *)"down", NULL};
		run(argv, NULL, NULL, 0);
		W.nnets = 0;
	}
	W.want[0] = 0;
	W.poll_t = 0.3f;
}

int sys_wifi_scan(net_t *out, int max)
{
	int n = W.nnets < max ? W.nnets : max;
	memcpy(out, W.nets, n * sizeof *out);
	return n;
}

void sys_wifi_rescan(void)
{
	if (!wifi_running()) {
		if (!sys_wifi_enabled())
			return;
		conf_ensure();
		sh(WIFI_INIT, "start");
	}
	wpa("scan", NULL, NULL, 0);
	W.scan_t = 3.0f;
}

int sys_wifi_scanning(void)
{
	return W.scan_t > 0;
}

void sys_wifi_connect(const char *ssid, const char *psk)
{
	if (!ssid || !*ssid || strlen(ssid) > 32 || strchr(ssid, '"') || strchr(ssid, '\n')) {
		W.state = WIFI_FAILED;
		W.err = "UNSUPPORTED NAME";
		return;
	}
	conf_ensure();
	char block[512];
	if (psk) {	// derive the key; the password only travels on a pipe
		if (strlen(psk) < 8 || strlen(psk) > 63) {
			W.state = WIFI_FAILED;
			W.err = "PASSWORD MUST BE 8-63 CHARACTERS";
			return;
		}
		char in[80], out[512];
		snprintf(in, sizeof in, "%s\n", psk);
		char *const argv[] = {(char *)"/usr/sbin/wpa_passphrase", (char *)ssid, NULL};
		run(argv, in, out, sizeof out);
		memset(in, 0, sizeof in);
		char *k = strstr(out, "\n\tpsk=");
		char hex[65] = "";
		if (k && sscanf(k + 6, "%64[0-9a-f]", hex) != 1)
			hex[0] = 0;
		memset(out, 0, sizeof out);
		if (strlen(hex) != 64) {
			W.state = WIFI_FAILED;
			W.err = "COULDN'T USE THAT PASSWORD";
			return;
		}
		int prio = conf_replace(ssid, NULL) + 1;
		snprintf(block, sizeof block, "network={\n\tssid=\"%s\"\n\tpsk=%s\n\tpriority=%d\n}\n", ssid, hex, prio);
		conf_replace(ssid, block);
		memset(block, 0, sizeof block);
	} else if (!conf_has(ssid)) {	// open network
		int prio = conf_replace(ssid, NULL) + 1;
		snprintf(block, sizeof block, "network={\n\tssid=\"%s\"\n\tkey_mgmt=NONE\n\tpriority=%d\n}\n", ssid, prio);
		conf_replace(ssid, block);
	}
	touch(WIFI_OFF_FLAG, 0);
	if (wifi_running()) {
		wpa("reconfigure", NULL, NULL, 0);
		// prefer this network now: find its id and select it
		char out[2048], idbuf[16] = "";
		wpa("list_networks", NULL, out, sizeof out);
		for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {
			int id;
			char s[64];
			if (sscanf(l, "%d\t%63[^\t]", &id, s) == 2 && !strcmp(s, ssid))
				snprintf(idbuf, sizeof idbuf, "%d", id);
		}
		if (idbuf[0]) {
			wpa("select_network", idbuf, NULL, 0);
			wpa("enable_network", "all", NULL, 0);	// keep the others for later
		}
	} else {
		sh(WIFI_INIT, "start");
	}
	snprintf(W.want, sizeof W.want, "%s", ssid);
	W.connect_t = 0;
	W.saw_handshake = 0;
	W.err = NULL;
	W.state = WIFI_CONNECTING;
	W.poll_t = 0.5f;
}

void sys_wifi_forget(const char *ssid)
{
	conf_replace(ssid, NULL);
	if (wifi_running())
		wpa("reconfigure", NULL, NULL, 0);
	for (int i = 0; i < W.nnets; i++)
		if (!strcmp(W.nets[i].ssid, ssid))
			W.nets[i].saved = 0;
	W.poll_t = 0.3f;
}

int sys_wifi_status(const char **ssid, const char **ip)
{
	if (ssid)
		*ssid = W.want[0] ? W.want : W.ssid;
	if (ip)
		*ip = W.ip;
	if (!sys_wifi_enabled())
		return WIFI_OFF;
	return W.state;
}

const char *sys_wifi_error(void)
{
	return W.err ? W.err : "";
}

/* ---- SSH ---------------------------------------------------------------------- */

int sys_ssh_enabled(void)
{
	return access(SSH_OFF_FLAG, F_OK) != 0;
}

void sys_ssh_enable(int on)
{
	touch(SSH_OFF_FLAG, !on);
	sh(SSH_INIT, on ? "restart" : "stop");
}

int sys_ssh_password_login(void)
{
	return access(SSH_PW_FLAG, F_OK) == 0;
}

void sys_ssh_set_password_login(int on)
{
	touch(SSH_PW_FLAG, on);
	if (sys_ssh_enabled())
		sh(SSH_INIT, "restart");
}

int sys_ssh_keys(void)
{
	FILE *f = fopen("/root/.ssh/authorized_keys", "r");
	if (!f)
		return 0;
	char line[4096];
	int n = 0;
	while (fgets(line, sizeof line, f))
		if (line[0] && line[0] != '#' && line[0] != '\n')
			n++;
	fclose(f);
	return n;
}

int sys_ssh_has_password(void)
{
	return 0;	// the root filesystem is read-only: no root password yet
}

int sys_ssh_set_password(const char *pw)
{
	(void)pw;
	return -1;	// TODO: /etc/shadow on /data
}

const char *sys_ssh_fingerprint(void)
{
	static char fp[96];
	if (fp[0])
		return fp;
	char out[2048];
	char *const argv[] = {(char *)"/usr/bin/dropbearkey", (char *)"-y", (char *)"-f",
			      (char *)"/data/dropbear/dropbear_ed25519_host_key", NULL};
	run(argv, NULL, out, sizeof out);
	char *k = strstr(out, "Fingerprint: ");
	if (k) {
		snprintf(fp, sizeof fp, "%s", k + 13);
		fp[strcspn(fp, "\n")] = 0;
	}
	return fp[0] ? fp : "UNKNOWN";
}

const char *sys_usb_ip(void)
{
	static char ip[INET_ADDRSTRLEN];
	ip[0] = 0;
	struct ifaddrs *ifa;
	if (getifaddrs(&ifa) == 0) {
		for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !strcmp(i->ifa_name, "usb0"))
				inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
		freeifaddrs(ifa);
	}
	return ip;
}

/* ---- PC link (the web app) ---------------------------------------------------- */
//
// cozisboy-web runs only while the PC LINK page is open: started on entering,
// told to quit on leaving, and kept alive by check-ins (it exits by itself
// ~10 s after the last one). Code, browsers and pairing go through its
// local-only endpoints (/api/local/...).

// One request to the web app on 127.0.0.1; the JSON body into out.
static int web_local(const char *method, const char *what, char *out, size_t outlen)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct timeval tv = {.tv_sec = 1};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80), .sin_addr.s_addr = htonl(0x7f000001)};
	char req[200], buf[1024];
	int n = snprintf(req, sizeof req, "%s /api/local/%s HTTP/1.0\r\nX-Cozis: 1\r\nContent-Length: 0\r\n\r\n", method, what);
	size_t got = 0;
	ssize_t r;
	if (connect(fd, (struct sockaddr *)&a, sizeof a) || write(fd, req, n) != n) {
		close(fd);
		return -1;
	}
	while (got < sizeof buf - 1 && (r = read(fd, buf + got, sizeof buf - 1 - got)) > 0)
		got += r;
	close(fd);
	buf[got] = 0;
	char *body = strstr(buf, "\r\n\r\n");
	if (strncmp(buf, "HTTP/1.1 200", 12) || !body)
		return -1;
	if (out)
		snprintf(out, outlen, "%s", body + 4);
	return 0;
}

static struct {
	char code[8];
	int browsers, pairs, up, mode;
	float next;
} WEB;

static void web_parse(const char *j)
{
	const char *c = strstr(j, "\"code\":\""), *b = strstr(j, "\"browsers\":"), *p = strstr(j, "\"pairs\":");
	if (c)
		sscanf(c + 8, "%6[0-9]", WEB.code);
	if (b)
		WEB.browsers = atoi(b + 11);
	if (p)
		WEB.pairs = atoi(p + 8);
}

static void web_checkin(void)
{
	static const char *const mode[] = {"off", "usb", "wifi"};
	char what[40], j[256];
	snprintf(what, sizeof what, "pairing?mode=%s", mode[WEB.mode]);
	WEB.up = web_local("POST", what, j, sizeof j) == 0;
	if (WEB.up)
		web_parse(j);
}

void sys_web_open(void)
{
	memset(&WEB, 0, sizeof WEB);
	mkdir("/data/library", 0755);
	if (web_local("GET", "info", NULL, 0) != 0) {	// not running yet: start it
		// double fork: the server is reparented to init and never waited for
		pid_t p = fork();
		if (p == 0) {
			if (fork() == 0) {
				setsid();
				int nul = open("/dev/null", O_RDWR);
				int log = open("/run/cozisboy-web.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
				dup2(nul, 0), dup2(log >= 0 ? log : nul, 1), dup2(log >= 0 ? log : nul, 2);
				for (int fd = 3; fd < 1024; fd++)	// not the launcher's devices (sound, ...)
					close(fd);
				execl("/usr/sbin/cozisboy-web", "cozisboy-web", "80", (char *)NULL);
				_exit(127);
			}
			_exit(0);
		}
		if (p > 0)
			waitpid(p, NULL, 0);	// the middle process exits at once
		for (int i = 0; i < 20 && web_local("GET", "info", NULL, 0) != 0; i++)
			usleep(50000);
	}
	web_checkin();
}

void sys_web_close(void)
{
	if (WEB.up)
		web_local("POST", "quit", NULL, 0);
	memset(&WEB, 0, sizeof WEB);
}

void sys_web_tick(int pairing)
{
	float now = ui_time();
	if (pairing != WEB.mode) {	// opened/closed a pairing screen: tell it now
		WEB.mode = pairing;
		WEB.next = 0;
	}
	if (now >= WEB.next || now + 5 < WEB.next) {
		WEB.next = now + 0.5f;	// check in twice a second (it gives up after ~4 s)
		web_checkin();
	}
}

int sys_web_running(void) { return WEB.up; }
const char *sys_web_code(void) { return WEB.code; }
// Paired browsers: from the server while it runs, else from its saved list.
#define WEB_SESSIONS "/data/web/sessions"
int sys_web_browsers(void)
{
	if (WEB.up)
		return WEB.browsers;
	static float last = -10;
	static int n;
	float now = ui_time();
	if (now - last > 1.0f || now < last) {
		last = now, n = 0;
		FILE *f = fopen(WEB_SESSIONS, "r");
		char line[64];
		while (f && fgets(line, sizeof line, f))
			n += strlen(line) >= 32;
		if (f)
			fclose(f);
	}
	return n;
}
int sys_web_pairs(void) { return WEB.pairs; }

void sys_web_forget_browsers(void)
{
	char j[256];
	if (WEB.up && web_local("POST", "forget", j, sizeof j) == 0)
		web_parse(j);
	else
		unlink(WEB_SESSIONS);	// not running: it reads the list when it starts
	sync();
}

// The USB device controller reaches "configured" once a computer has set up
// the link (cable in, the PC's driver bound).
int sys_usb_connected(void)
{
	char st[32] = "";
	FILE *f = NULL;
	DIR *d = opendir("/sys/class/udc");
	struct dirent *e;
	while (d && (e = readdir(d)) && !f)
		if (e->d_name[0] != '.') {
			char p[300];
			snprintf(p, sizeof p, "/sys/class/udc/%s/state", e->d_name);
			f = fopen(p, "r");
		}
	if (d)
		closedir(d);
	if (f) {
		if (!fgets(st, sizeof st, f))
			st[0] = 0;
		fclose(f);
	}
	return !strncmp(st, "configured", 10);
}

/* ---- clock ---------------------------------------------------------------------- */

#define CLOCK_MANUAL "/data/etc/clock-manual"	// exists: no network time (S45wifi)

int sys_clock_auto(void)
{
	return access(CLOCK_MANUAL, F_OK) != 0;
}

int sys_clock_set_auto(int on)
{
	char *const kill_ntpd[] = {(char *)"/usr/bin/killall", (char *)"ntpd", NULL};
	run(kill_ntpd, NULL, NULL, 0);
	if (!on) {
		mkdir("/data/etc", 0755);
		int fd = open(CLOCK_MANUAL, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			return -1;
		close(fd);
		sync();
		return 0;
	}
	unlink(CLOCK_MANUAL);
	sync();
	// as S45wifi does: keeps trying until the network is up, then corrects
	// the clock and saves it to the RTC (the hook); ntpd daemonizes itself
	char *const ntpd[] = {(char *)"/usr/sbin/ntpd", (char *)"-p", (char *)"pool.ntp.org", (char *)"-S",
			      (char *)"/usr/libexec/cozisboy-ntp-hook", NULL};
	return run(ntpd, NULL, NULL, 0) == 0 ? 0 : -1;
}

int sys_clock_set(long t)
{
	struct timeval tv = {.tv_sec = t, .tv_usec = 0};
	if (settimeofday(&tv, NULL))
		return -1;
	// the battery-backed RTC keeps it while the console is off
	char *const save[] = {(char *)"/bin/sh", (char *)"-c",
			      (char *)"[ -r /run/rtc-device ] && hwclock -w -u -f \"$(cat /run/rtc-device)\"", NULL};
	run(save, NULL, NULL, 0);
	return 0;
}

/* ---- Bluetooth: cozisboy-bt, run in the background ------------------------------ */

#define BT_TOOL "/usr/bin/cozisboy-bt"

static struct {
	pid_t status_pid, act_pid;
	int status_fd;
	char buf[4096];
	size_t len;
	float t;
	int known, powered, visible;
	char ip[INET_ADDRSTRLEN];
	btdev_t devs[16];
	int ndev;
	const char *act_ok, *act_fail;	// toast for when the action ends
	const char *result;
	int want_refresh;
} B;

static pid_t bt_spawn(char *const argv[], int *out_fd)
{
	int pfd[2] = {-1, -1};
	if (out_fd && pipe(pfd))
		return -1;
	pid_t p = fork();
	if (p == 0) {
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0);
		dup2(out_fd ? pfd[1] : nul, 1);
		dup2(nul, 2);
		for (int fd = 3; fd < 256; fd++)
			close(fd);
		execv(argv[0], argv);
		_exit(127);
	}
	if (out_fd) {
		close(pfd[1]);
		if (p < 0) {
			close(pfd[0]);
			return -1;
		}
		fcntl(pfd[0], F_SETFL, O_NONBLOCK);
		*out_fd = pfd[0];
	}
	return p;
}

static void bt_parse(void)
{
	B.buf[B.len] = 0;
	B.ndev = 0;
	B.ip[0] = 0;
	for (char *l = strtok(B.buf, "\n"); l; l = strtok(NULL, "\n")) {
		if (!strncmp(l, "powered ", 8))
			B.powered = !strcmp(l + 8, "yes");
		else if (!strncmp(l, "visible ", 8))
			B.visible = atoi(l + 8);
		else if (!strncmp(l, "ip ", 3))
			snprintf(B.ip, sizeof B.ip, "%s", l + 3);
		else if (!strncmp(l, "dev\t", 4) && B.ndev < 16) {
			btdev_t *d = &B.devs[B.ndev];
			char *f[6] = {0};
			int n = 0;
			for (char *q = l; q && n < 6; n++) {
				f[n] = q;
				q = strchr(q, '\t');
				if (q)
					*q++ = 0;
			}
			if (n < 6)
				continue;
			snprintf(d->mac, sizeof d->mac, "%s", f[1]);
			d->connected = atoi(f[2]), d->nap = atoi(f[3]), d->internet = atoi(f[4]);
			snprintf(d->name, sizeof d->name, "%s", f[5]);
			for (char *q = d->name; *q; q++)
				*q = (char)toupper((unsigned char)*q);
			B.ndev++;
		}
	}
	B.known = 1;
}

void sys_bt_tick(float dt, int watching)
{
	if (access(BT_TOOL, X_OK))
		return;
	B.t += dt;
	// a finished action: its toast, and fresh status
	if (B.act_pid > 0) {
		int st;
		if (waitpid(B.act_pid, &st, WNOHANG) == B.act_pid) {
			B.result = WIFEXITED(st) && WEXITSTATUS(st) == 0 ? B.act_ok : B.act_fail;
			B.act_pid = 0;
			B.want_refresh = 1;
		}
	}
	// status being read
	if (B.status_pid > 0) {
		ssize_t n;
		while (B.len < sizeof B.buf - 1 && (n = read(B.status_fd, B.buf + B.len, sizeof B.buf - 1 - B.len)) > 0)
			B.len += (size_t)n;
		int st;
		if (waitpid(B.status_pid, &st, WNOHANG) == B.status_pid) {
			while (B.len < sizeof B.buf - 1 && (n = read(B.status_fd, B.buf + B.len, sizeof B.buf - 1 - B.len)) > 0)
				B.len += (size_t)n;
			close(B.status_fd);
			B.status_pid = 0;
			bt_parse();
		}
		return;
	}
	float every = watching ? (B.visible ? 1.0f : 2.0f) : 15.0f;
	if (B.act_pid == 0 && (B.want_refresh || !B.known || B.t >= every)) {
		char *const argv[] = {(char *)BT_TOOL, (char *)"status", NULL};
		B.len = 0, B.t = 0, B.want_refresh = 0;
		B.status_pid = bt_spawn(argv, &B.status_fd);
	}
}

static void bt_action(const char *a, const char *b, const char *ok, const char *fail)
{
	if (B.act_pid > 0)
		return;
	char *const argv[] = {(char *)BT_TOOL, (char *)a, (char *)b, NULL};
	B.act_ok = ok, B.act_fail = fail;
	B.act_pid = bt_spawn(argv, NULL);
}

int sys_bt_known(void) { return B.known; }
int sys_bt_powered(void) { return B.powered; }
int sys_bt_visible(void) { return B.visible; }
const char *sys_bt_ip(void) { return B.ip; }
int sys_bt_busy(void) { return B.act_pid > 0; }
int sys_bt_devices(btdev_t *out, int max)
{
	int n = B.ndev < max ? B.ndev : max;
	memcpy(out, B.devs, (size_t)n * sizeof *out);
	return n;
}
const char *sys_bt_result(void)
{
	const char *r = B.result;
	B.result = NULL;
	return r;
}
void sys_bt_power(int on)
{
	B.powered = on;	// show it at once; the next status confirms
	bt_action("power", on ? "on" : "off", on ? "BLUETOOTH ON" : "BLUETOOTH OFF", "COULDN'T CHANGE BLUETOOTH");
}
void sys_bt_make_visible(void)
{
	B.visible = 180;
	bt_action("visible", NULL, NULL, "COULDN'T MAKE IT VISIBLE");
}
void sys_bt_hide(void)
{
	B.visible = 0;
	bt_action("hidden", NULL, "NOT VISIBLE ANY MORE", NULL);
}
void sys_bt_forget(const char *mac)
{
	static char m[18];
	snprintf(m, sizeof m, "%s", mac);
	bt_action("forget", m, "DEVICE FORGOTTEN", "COULDN'T FORGET IT");
}
void sys_bt_internet(const char *mac, int on)
{
	static char m[18];
	snprintf(m, sizeof m, "%s", mac);
	if (on)
		bt_action("internet", m, "USING THE PHONE'S INTERNET", "NO INTERNET: TURN ON BLUETOOTH TETHERING ON THE PHONE");
	else
		bt_action("internet-off", NULL, "STOPPED USING ITS INTERNET", NULL);
}
