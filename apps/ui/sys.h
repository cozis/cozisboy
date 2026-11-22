// System services the UI configures: Wi-Fi and the SSH server.
// sys_mock.c fakes them for the PC preview; the device implementation will
// talk to the Wi-Fi daemon and start/stop dropbear.
#ifndef SYS_H
#define SYS_H

typedef struct {
	char ssid[33];
	int bars;   // signal 1..4
	int secure; // needs a password
	int saved;  // password remembered
} net_t;

enum { WIFI_OFF, WIFI_DISCONNECTED, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };

void sys_tick(float dt); // advance background work (connects, scans)

int sys_wifi_enabled(void);
void sys_wifi_enable(int on);
int sys_wifi_scan(net_t *out, int max); // last scan results, strongest first
void sys_wifi_rescan(void);
int sys_wifi_scanning(void);
// Connect in the background; psk may be NULL for open or saved networks.
void sys_wifi_connect(const char *ssid, const char *psk);
void sys_wifi_forget(const char *ssid);
// State, plus the network (ssid) and address (ip) when connected.
int sys_wifi_status(const char **ssid, const char **ip);
const char *sys_wifi_error(void); // why the last connect failed

int sys_ssh_enabled(void);
void sys_ssh_enable(int on);
int sys_ssh_password_login(void);
void sys_ssh_set_password_login(int on);
int sys_ssh_keys(void);                 // authorized keys installed
int sys_ssh_has_password(void);         // root has a password set
// 0 on success; NULL removes the password (locks password login for root).
// Implementations must store only a hash (chpasswd -> /etc/shadow),
// pass the password through a pipe (never argv), and never log or keep it.
int sys_ssh_set_password(const char *pw);
const char *sys_ssh_fingerprint(void);  // host key fingerprint
const char *sys_usb_ip(void);           // address over the USB cable

// PC link: the web app (apps/web) runs only while the PC LINK page is open.
// Browsers paired before connect again then; a new one pairs from the page:
// over USB by itself, over Wi-Fi with the code shown on the page.
enum { WEB_PAIR_NONE, WEB_PAIR_USB, WEB_PAIR_WIFI };
void sys_web_open(void);            // start it (entering the page)
void sys_web_close(void);           // stop it (leaving the page)
void sys_web_tick(int pairing);     // every frame on the page: keeps it (and pairing) open
int sys_web_running(void);
const char *sys_web_code(void);     // the Wi-Fi pairing code ("" until known)
int sys_web_browsers(void);         // paired browsers
int sys_web_pairs(void);            // pairings so far: goes up when a browser pairs
void sys_web_forget_browsers(void); // every browser has to pair again
int sys_usb_connected(void);        // the USB cable is plugged into a computer

// Clock. Automatic: network time (ntpd) whenever Wi-Fi is up; off, the time
// is whatever was set by hand. Setting it also saves it to the battery-backed RTC.
int sys_clock_auto(void);
int sys_clock_set_auto(int on);
int sys_clock_set(long t);	// seconds since 1970 (UTC)

#endif
