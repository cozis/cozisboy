// bt-pan <device address>: join the phone's Bluetooth tethering (PAN NAP) and
// keep the link up until killed (BlueZ drops it when the asking client goes).
// Prints the network interface (bnep0).
#include <dbus/dbus.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	if (argc != 2) { fprintf(stderr, "usage: bt-pan AA:BB:CC:DD:EE:FF\n"); return 2; }
	char path[64];
	snprintf(path, sizeof path, "/org/bluez/hci0/dev_%s", argv[1]);
	for (char *p = path; *p; p++) if (*p == ':') *p = '_';
	DBusError err; dbus_error_init(&err);
	DBusConnection *c = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
	if (!c) { fprintf(stderr, "dbus: %s\n", err.message); return 1; }
	DBusMessage *m = dbus_message_new_method_call("org.bluez", path, "org.bluez.Network1", "Connect");
	const char *role = "nap";
	dbus_message_append_args(m, DBUS_TYPE_STRING, &role, DBUS_TYPE_INVALID);
	DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 30000, &err);
	if (!r) { fprintf(stderr, "connect: %s\n", err.message); return 1; }
	const char *iface = NULL;
	dbus_message_get_args(r, &err, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
	printf("%s\n", iface ? iface : "?"); fflush(stdout);
	while (dbus_connection_read_write(c, -1)) // stay on the bus
		while (dbus_connection_pop_message(c)) ;
	return 0;
}
