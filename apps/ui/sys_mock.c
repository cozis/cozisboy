// Fake Wi-Fi and SSH for the PC preview: a few made-up networks, connects that
// take a moment, and a password check (8+ characters) so errors can be seen.
#include "sys.h"
float ui_time(void); // ui.c (not ui_priv.h: its W/H macros)

#include <stdio.h>
#include <string.h>

static net_t nets[] = {
	{"HOME-NET", 4, 1, 1},
	{"CozyCafe", 3, 0, 0},
	{"Neighbor-5G", 2, 1, 0},
	{"PRINTER-direct", 1, 1, 0},
	{"Free_WiFi", 1, 0, 0},
};
#define NNETS ((int)(sizeof nets / sizeof *nets))

static struct {
	int on, state;
	char ssid[33], ip[20], pending_psk[64];
	float t, scan_t;
	const char *err;
	int ssh, ssh_pw, has_pw;
} M = {1, WIFI_CONNECTED, "HOME-NET", "192.168.1.42", "", 0, 0, NULL, 1, 0, 0};

void sys_tick(float dt)
{
	if (M.scan_t > 0)
		M.scan_t -= dt;
	if (M.state != WIFI_CONNECTING)
		return;
	M.t += dt;
	if (M.t < 1.4f)
		return;
	net_t *n = NULL;
	for (int i = 0; i < NNETS; i++)
		if (!strcmp(nets[i].ssid, M.ssid))
			n = &nets[i];
	if (n && n->secure && !n->saved && strlen(M.pending_psk) < 8) {
		M.state = WIFI_FAILED;
		M.err = "WRONG PASSWORD";
		return;
	}
	if (n && n->secure)
		n->saved = 1;
	M.state = WIFI_CONNECTED;
	snprintf(M.ip, sizeof M.ip, "192.168.1.%d", 20 + (int)(strlen(M.ssid) * 7 % 200));
}

int sys_wifi_enabled(void)
{
	return M.on;
}

void sys_wifi_enable(int on)
{
	M.on = on;
	M.state = on ? WIFI_DISCONNECTED : WIFI_OFF;
	if (on) { // auto-join the saved network
		strcpy(M.ssid, "HOME-NET");
		M.state = WIFI_CONNECTING;
		M.t = 0.6f;
		M.pending_psk[0] = 0;
	}
}

int sys_wifi_scan(net_t *out, int max)
{
	if (!M.on || M.scan_t > 0)
		return 0;
	int n = NNETS < max ? NNETS : max;
	memcpy(out, nets, (size_t)n * sizeof *out);
	return n;
}

void sys_wifi_rescan(void)
{
	M.scan_t = 1.0f;
}

int sys_wifi_scanning(void)
{
	return M.on && M.scan_t > 0;
}

void sys_wifi_connect(const char *ssid, const char *psk)
{
	snprintf(M.ssid, sizeof M.ssid, "%s", ssid);
	snprintf(M.pending_psk, sizeof M.pending_psk, "%s", psk ? psk : "");
	M.state = WIFI_CONNECTING;
	M.t = 0;
	M.err = NULL;
}

void sys_wifi_forget(const char *ssid)
{
	for (int i = 0; i < NNETS; i++)
		if (!strcmp(nets[i].ssid, ssid))
			nets[i].saved = 0;
	if (!strcmp(M.ssid, ssid) && M.state == WIFI_CONNECTED)
		M.state = WIFI_DISCONNECTED;
}

int sys_wifi_status(const char **ssid, const char **ip)
{
	if (ssid)
		*ssid = M.ssid;
	if (ip)
		*ip = M.ip;
	return M.on ? M.state : WIFI_OFF;
}

const char *sys_wifi_error(void)
{
	return M.err ? M.err : "COULD NOT CONNECT";
}

int sys_ssh_enabled(void)
{
	return M.ssh;
}

void sys_ssh_enable(int on)
{
	M.ssh = on;
}

int sys_ssh_password_login(void)
{
	return M.ssh_pw;
}

void sys_ssh_set_password_login(int on)
{
	M.ssh_pw = on;
}

int sys_ssh_has_password(void)
{
	return M.has_pw;
}

int sys_ssh_set_password(const char *pw)
{
	if (!pw) {
		M.has_pw = 0, M.ssh_pw = 0;
		return 0;
	}
	if (strlen(pw) < 8)
		return -1;
	M.has_pw = 1; // the device implementation sets root's password (chpasswd)
	return 0;
}

int sys_ssh_keys(void)
{
	return 1;
}

const char *sys_ssh_fingerprint(void)
{
	return "SHA256:Qx3k9PzLw2fT7vB1mN8cR4yH6dJ0sKeU5aGiZoVt";
}

const char *sys_usb_ip(void)
{
	return "10.55.0.2";
}

/* ---- PC link (fake): USB "plugs in" 2 s after USB pairing starts, a browser
 * pairs 3 s after a pairing screen is ready ---- */
static int web_up, web_browsers = 1, web_pairs, web_mode;
static float web_t;
static char web_code[8] = "482913";
void sys_web_open(void) { web_up = 1, web_mode = 0; }
void sys_web_close(void) { web_up = 0; }
void sys_web_tick(int pairing)
{
	static float last;
	float now = ui_time(), dt = now - last;
	last = now;
	if (pairing != web_mode)
		web_mode = pairing, web_t = 0;
	if (pairing) {
		web_t += dt > 0.1f ? 0.1f : dt;
		if (web_t > (pairing == 1 ? 5.0f : 3.0f))
			web_pairs++, web_browsers++, web_t = -1000;
	}
}
int sys_web_running(void) { return web_up; }
const char *sys_web_code(void) { return web_up ? web_code : ""; }
int sys_web_browsers(void) { return web_browsers; }
int sys_web_pairs(void) { return web_pairs; }
void sys_web_forget_browsers(void) { web_browsers = 0; }
int sys_usb_connected(void)	// "plugged in" 2 s after something starts waiting for it
{
	static float first, lastcall = -10;
	float now = ui_time();
	if (now - lastcall > 0.5f)
		first = now;
	lastcall = now;
	return now - first > 2.0f;
}
