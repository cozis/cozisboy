// coz-lib: command-line access to the game library (tests, SSH use).
//   coz-lib list
//   coz-lib add FILE...            import ROMs (system from extension + header)
//   coz-lib delete ID
//   coz-lib rename ID NEW_NAME
//   coz-lib states ID
//   coz-lib state-test ID          add a dummy state with a gradient screenshot
#define _GNU_SOURCE
#include "library.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int copy_into(int in, int out)
{
	char buf[65536];
	ssize_t n;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, n) != n)
			return -EIO;
	return n < 0 ? -errno : 0;
}

static const coz_game *find(coz_lib *lib, const char *id)
{
	const coz_game *g = coz_lib_find(lib, id);
	if (!g)
		fprintf(stderr, "no game %s\n", id);
	return g;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: coz-lib list|add|delete|rename|states|state-test ...\n");
		return 2;
	}
	const char *cmd = argv[1];
	coz_lib lib;
	int r;

	if (!strcmp(cmd, "add")) {
		int fails = 0;
		for (int i = 2; i < argc; i++) {
			coz_import imp;
			coz_game g;
			int in = open(argv[i], O_RDONLY);
			if (in < 0 || (r = coz_import_begin(&imp, argv[i]))) {
				fprintf(stderr, "skip %s: %s\n", argv[i], in < 0 ? strerror(errno) : strerror(-r));
				if (in >= 0)
					close(in);
				fails++;
				continue;
			}
			r = copy_into(in, imp.fd);
			close(in);
			if (r) {
				coz_import_abort(&imp);
			} else
				r = coz_import_commit(&imp, 0, &g);
			if (r) {
				fprintf(stderr, "fail %s: %s\n", argv[i], strerror(-r));
				fails++;
			} else
				printf("%s  %-3s  %s\n", g.id, coz_sys_short(g.sys), g.name);
		}
		return fails ? 1 : 0;
	}

	if ((r = coz_lib_scan(&lib))) {
		fprintf(stderr, "scan: %s\n", strerror(-r));
		return 1;
	}
	if (!strcmp(cmd, "list")) {
		for (int i = 0; i < lib.n; i++) {
			const coz_game *g = &lib.games[i];
			printf("%s  %-3s  %-40.40s  %-14.14s %8lld KB  save:%s states:%d cover:%s\n",
			       g->id, coz_sys_short(g->sys), g->title, g->region, g->rom_size / 1024,
			       g->has_save ? "yes" : "no", g->nstates, g->has_cover ? "yes" : "no");
		}
		printf("%d games in %s\n", lib.n, coz_lib_root());
	} else if (!strcmp(cmd, "delete") && argc == 3) {
		const coz_game *g = find(&lib, argv[2]);
		if (!g || (r = coz_game_delete(g)))
			return g ? (fprintf(stderr, "%s\n", strerror(-r)), 1) : 1;
	} else if (!strcmp(cmd, "rename") && argc == 4) {
		const coz_game *g = find(&lib, argv[2]);
		if (!g)
			return 1;
		coz_game c = *g;
		if ((r = coz_game_rename(&c, argv[3]))) {
			fprintf(stderr, "%s\n", strerror(-r));
			return 1;
		}
		printf("%s  %s\n", c.id, c.name);
	} else if (!strcmp(cmd, "states") && argc == 3) {
		const coz_game *g = find(&lib, argv[2]);
		coz_state *s;
		int n;
		if (!g || coz_states_list(g, &s, &n))
			return 1;
		for (int i = 0; i < n; i++)
			printf("%s  %lld bytes  thumb:%s\n", s[i].id, s[i].size, s[i].has_thumb ? "yes" : "no");
		free(s);
	} else if (!strcmp(cmd, "state-test") && argc == 3) {
		const coz_game *g = find(&lib, argv[2]);
		if (!g)
			return 1;
		uint32_t px[240 * 160];
		for (int y = 0; y < 160; y++)
			for (int x = 0; x < 240; x++)
				px[y * 240 + x] = (x * 255 / 239) << 16 | (y * 255 / 159) << 8 | 0x80;
		coz_state s;
		if ((r = coz_state_add(g, "dummy state", 11, px, 240, 160, 240, &s))) {
			fprintf(stderr, "%s\n", strerror(-r));
			return 1;
		}
		printf("%s\n", s.id);
	} else {
		fprintf(stderr, "bad command\n");
		return 2;
	}
	coz_lib_free(&lib);
	return 0;
}
