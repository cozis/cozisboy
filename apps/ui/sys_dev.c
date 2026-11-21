// System services on the device: Wi-Fi through wpa_supplicant (S45wifi, networks
// in /data/wifi/wpa_supplicant.conf) and the dropbear SSH server.
//
// Wi-Fi passwords never reach a command line: wpa_passphrase reads them on a
// pipe and only the derived key (psk=<hex>) is written to the config file.
#include "sys.h"
float ui_time(void); // ui.c (not ui_priv.h: its W/H macros)

#include <arpa/inet.h>
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
// cozisboy-web (S60web) runs unless /data/web-off exists. Its code and paired
// browsers come from its local-only endpoints (/api/local/...).

#define WEB_OFF_FLAG "/data/web-off"
#define WEB_INIT "/etc/init.d/S60web"

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
	char req[160], buf[1024];
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
	int browsers;
} WEB;

static void web_refresh(void)
{
	char j[256];
	WEB.code[0] = 0, WEB.browsers = 0;
	if (web_local("GET", "info", j, sizeof j))
		return;
	char *c = strstr(j, "\"code\":\""), *b = strstr(j, "\"browsers\":");
	if (c)
		sscanf(c + 8, "%6[0-9]", WEB.code);
	if (b)
		WEB.browsers = atoi(b + 11);
}

int sys_web_enabled(void)
{
	return access(WEB_OFF_FLAG, F_OK) != 0;
}

void sys_web_enable(int on)
{
	touch(WEB_OFF_FLAG, !on);
	sh(WEB_INIT, on ? "start" : "stop");
	if (on)
		usleep(200000);	// let it start listening
	web_refresh();
}

const char *sys_web_code(void)
{
	static float last;
	float now = ui_time();
	if (now - last > 1.0f || now < last) {	// at most once a second
		last = now;
		web_refresh();
	}
	return WEB.code;
}

int sys_web_browsers(void)
{
	sys_web_code();
	return WEB.browsers;
}

void sys_web_new_code(void)
{
	web_local("POST", "newcode", NULL, 0);
	web_refresh();
}

void sys_web_forget_browsers(void)
{
	web_local("POST", "forget", NULL, 0);
	web_refresh();
}
