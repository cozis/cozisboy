// cozisboy game library, see library.h.
//
// Storage (internal, may change):
//   <root>/<sys>/<name>/rom.<ext>        the ROM
//   <root>/<sys>/<name>/save.sav         battery save
//   <root>/<sys>/<name>/save.bak         previous battery save
//   <root>/<sys>/<name>/save<n>.sav/.bak extra saves 2..COZ_SAVE_SLOTS
//   <root>/<sys>/<name>/cover.png        box art
//   <root>/<sys>/<name>/states/<id>.state + <id>.png
//   <root>/<sys>/<name>/info             "added=<unix time>"
//   <root>/.tmp/                         imports in progress (same filesystem,
//                                        so commits are atomic renames)
// One folder per game: rename and delete move everything together.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <png.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_ROOT "/data/library"

static const struct {
	const char *key, *label, *shrt, *ext;
} SYS[COZ_SYS_COUNT] = {
	[COZ_SYS_GB] = { "gb", "Game Boy", "GB", "gb" },
	[COZ_SYS_GBC] = { "gbc", "Game Boy Color", "GBC", "gbc" },
	[COZ_SYS_GBA] = { "gba", "Game Boy Advance", "GBA", "gba" },
};

const char *coz_sys_key(coz_system s) { return s < COZ_SYS_COUNT ? SYS[s].key : "?"; }
const char *coz_sys_label(coz_system s) { return s < COZ_SYS_COUNT ? SYS[s].label : "?"; }
const char *coz_sys_short(coz_system s) { return s < COZ_SYS_COUNT ? SYS[s].shrt : "?"; }

int coz_sys_from_filename(const char *filename)
{
	const char *dot = strrchr(filename, '.');
	if (!dot)
		return -1;
	for (int s = 0; s < COZ_SYS_COUNT; s++)
		if (!strcasecmp(dot + 1, SYS[s].ext))
			return s;
	return -1;
}

const char *coz_lib_root(void)
{
	const char *r = getenv("COZ_LIBRARY");
	return r && *r ? r : DEFAULT_ROOT;
}

// ------------------------------------------------------------------ helpers

static int valid_name(const char *n)
{
	size_t l = strlen(n);
	if (!l || l > COZ_NAME_MAX || n[0] == '.' || n[0] == ' ' || n[l - 1] == ' ')
		return 0;
	for (const unsigned char *p = (const unsigned char *)n; *p; p++)
		if (*p < 0x20 || *p == '/' || *p == '\\' || *p == 0x7f)
			return 0;
	return 1;
}

static void make_id(coz_system s, const char *name, char *id)
{
	uint64_t h = 0xcbf29ce484222325ull;	// FNV-1a 64
	const char *k = SYS[s].key;
	for (const char *p = k; *p; p++)
		h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
	h = (h ^ '/') * 0x100000001b3ull;
	for (const char *p = name; *p; p++)
		h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
	snprintf(id, COZ_ID_LEN, "%016llx", (unsigned long long)h);
}

// "Pokemon - Emerald Version (USA, Europe) (Rev 1)" -> title + first tag
static void split_name(const char *name, char *title, char *region)
{
	const char *cut = NULL;
	for (const char *p = name; *p; p++)
		if (p[0] == ' ' && (p[1] == '(' || p[1] == '[')) {
			cut = p;
			break;
		}
	size_t tl = cut ? (size_t)(cut - name) : strlen(name);
	memcpy(title, name, tl);
	title[tl] = 0;
	region[0] = 0;
	if (cut && cut[1] == '(') {
		const char *e = strchr(cut + 2, ')');
		if (e && e - (cut + 2) < 63) {
			memcpy(region, cut + 2, e - (cut + 2));
			region[e - (cut + 2)] = 0;
		}
	}
}

static int game_dir(coz_system s, const char *name, char *buf, size_t len)
{
	int n = snprintf(buf, len, "%s/%s/%s", coz_lib_root(), SYS[s].key, name);
	return n < 0 || (size_t)n >= len ? -ENAMETOOLONG : 0;
}

static int game_file(const coz_game *g, const char *file, char *buf, size_t len)
{
	int n = snprintf(buf, len, "%s/%s/%s/%s", coz_lib_root(), SYS[g->sys].key, g->name, file);
	return n < 0 || (size_t)n >= len ? -ENAMETOOLONG : 0;
}

static int mkdirs(const char *path)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) && errno != EEXIST)
				return -errno;
			*p = '/';
		}
	if (mkdir(tmp, 0755) && errno != EEXIST)
		return -errno;
	return 0;
}

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *f)
{
	(void)st, (void)flag, (void)f;
	return remove(p);
}

static int rm_tree(const char *path)
{
	return nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS) ? -errno : 0;
}

static void fsync_dir(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

static void parent_of(const char *path, char *out, size_t len)
{
	snprintf(out, len, "%s", path);
	char *s = strrchr(out, '/');
	if (s)
		*s = 0;
}

// ---------------------------------------------------------------- reading

static int find_rom(const char *dir, char *ext_out)
{
	for (int s = 0; s < COZ_SYS_COUNT; s++) {
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/rom.%s", dir, SYS[s].ext);
		if (access(p, F_OK) == 0) {
			strcpy(ext_out, SYS[s].ext);
			return 0;
		}
	}
	return -ENOENT;
}

int coz_game_refresh(coz_game *g)
{
	char dir[PATH_MAX], p[PATH_MAX], ext[8];
	struct stat st;
	if (game_dir(g->sys, g->name, dir, sizeof dir) || find_rom(dir, ext))
		return -ENOENT;
	make_id(g->sys, g->name, g->id);
	split_name(g->name, g->title, g->region);

	snprintf(p, sizeof p, "%s/rom.%s", dir, ext);
	if (stat(p, &st))
		return -errno;
	g->rom_size = st.st_size;
	g->added = st.st_mtime;
	snprintf(p, sizeof p, "%s/info", dir);
	FILE *f = fopen(p, "r");
	if (f) {
		long long t;
		if (fscanf(f, "added=%lld", &t) == 1)
			g->added = t;
		fclose(f);
	}

	snprintf(p, sizeof p, "%s/save.sav", dir);
	g->has_save = stat(p, &st) == 0;
	g->save_size = g->has_save ? st.st_size : 0;
	g->save_time = g->has_save ? st.st_mtime : 0;
	snprintf(p, sizeof p, "%s/save.bak", dir);
	g->has_save_backup = access(p, F_OK) == 0;
	g->save_slots = g->has_save || g->has_save_backup;
	for (int n = 2; n <= COZ_SAVE_SLOTS; n++) {
		snprintf(p, sizeof p, "%s/save%d.sav", dir, n);
		int any = access(p, F_OK) == 0;
		snprintf(p, sizeof p, "%s/save%d.bak", dir, n);
		if (any || access(p, F_OK) == 0)
			g->save_slots |= 1u << (n - 1);
	}
	snprintf(p, sizeof p, "%s/cover.png", dir);
	g->has_cover = stat(p, &st) == 0;
	g->cover_time = g->has_cover ? st.st_mtime : 0;

	g->nstates = 0;
	g->last_state = 0;
	coz_state *sv;
	int n;
	if (coz_states_list(g, &sv, &n) == 0) {
		g->nstates = n;
		g->last_state = n ? sv[0].time : 0;
		free(sv);
	}
	return 0;
}

static int cmp_games(const void *a, const void *b)
{
	const coz_game *x = a, *y = b;
	if (x->sys != y->sys)
		return (int)x->sys - (int)y->sys;
	int c = strcasecmp(x->title, y->title);
	return c ? c : strcmp(x->name, y->name);
}

int coz_lib_scan(coz_lib *lib)
{
	lib->games = NULL;
	lib->n = 0;
	int cap = 0;
	for (int s = 0; s < COZ_SYS_COUNT; s++) {
		char sd[PATH_MAX];
		snprintf(sd, sizeof sd, "%s/%s", coz_lib_root(), SYS[s].key);
		DIR *d = opendir(sd);
		if (!d)
			continue;
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.' || !valid_name(e->d_name))
				continue;
			if (lib->n == cap) {
				cap = cap ? cap * 2 : 64;
				coz_game *ng = realloc(lib->games, cap * sizeof *ng);
				if (!ng) {
					closedir(d);
					return -ENOMEM;
				}
				lib->games = ng;
			}
			coz_game *g = &lib->games[lib->n];
			memset(g, 0, sizeof *g);
			g->sys = s;
			snprintf(g->name, sizeof g->name, "%s", e->d_name);
			if (coz_game_refresh(g) == 0)
				lib->n++;
		}
		closedir(d);
	}
	if (lib->n)
		qsort(lib->games, lib->n, sizeof *lib->games, cmp_games);
	return 0;
}

void coz_lib_free(coz_lib *lib)
{
	free(lib->games);
	lib->games = NULL;
	lib->n = 0;
}

const coz_game *coz_lib_find(const coz_lib *lib, const char *id)
{
	for (int i = 0; i < lib->n; i++)
		if (!strcmp(lib->games[i].id, id))
			return &lib->games[i];
	return NULL;
}

int coz_game_rom_path(const coz_game *g, char *buf, size_t len)
{
	char f[16];
	snprintf(f, sizeof f, "rom.%s", SYS[g->sys].ext);
	return game_file(g, f, buf, len);
}
static int slot_file(const coz_game *g, int slot, const char *ext, char *buf, size_t len)
{
	if (slot < 1 || slot > COZ_SAVE_SLOTS)
		return -EINVAL;
	char f[24];
	if (slot == 1)
		snprintf(f, sizeof f, "save.%s", ext);
	else
		snprintf(f, sizeof f, "save%d.%s", slot, ext);
	return game_file(g, f, buf, len);
}
int coz_save_slot_path(const coz_game *g, int slot, char *buf, size_t len) { return slot_file(g, slot, "sav", buf, len); }
int coz_save_slot_backup_path(const coz_game *g, int slot, char *buf, size_t len) { return slot_file(g, slot, "bak", buf, len); }
int coz_game_save_path(const coz_game *g, char *buf, size_t len) { return slot_file(g, 1, "sav", buf, len); }
int coz_game_save_backup_path(const coz_game *g, char *buf, size_t len) { return slot_file(g, 1, "bak", buf, len); }
int coz_game_cover_path(const coz_game *g, char *buf, size_t len) { return game_file(g, "cover.png", buf, len); }

// ----------------------------------------------------------------- imports

static int tmp_begin(coz_import *imp)
{
	char d[PATH_MAX];
	snprintf(d, sizeof d, "%s/.tmp", coz_lib_root());
	int r = mkdirs(d);
	if (r)
		return r;
	snprintf(imp->tmp, sizeof imp->tmp, "%s/import-XXXXXX", d);
	imp->fd = mkostemp(imp->tmp, O_CLOEXEC);
	if (imp->fd < 0)
		return -errno;
	fchmod(imp->fd, 0644);
	return 0;
}

// Flush and close the temp file before it's renamed into place.
static int tmp_finish(coz_import *imp)
{
	int r = 0;
	if (fsync(imp->fd))
		r = -errno;
	if (close(imp->fd) && !r)
		r = -errno;
	imp->fd = -1;
	return r;
}

void coz_import_abort(coz_import *imp)
{
	if (imp->fd >= 0)
		close(imp->fd);
	imp->fd = -1;
	if (imp->tmp[0])
		unlink(imp->tmp);
	imp->tmp[0] = 0;
}

int coz_import_begin(coz_import *imp, const char *filename)
{
	memset(imp, 0, sizeof *imp);
	imp->fd = -1;
	const char *base = strrchr(filename, '/');
	base = base ? base + 1 : filename;
	imp->sys = coz_sys_from_filename(base);
	if (imp->sys < 0)
		return -EINVAL;
	size_t l = strrchr(base, '.') - base;
	if (l > COZ_NAME_MAX)
		return -ENAMETOOLONG;
	memcpy(imp->name, base, l);
	imp->name[l] = 0;
	if (!valid_name(imp->name))
		return -EINVAL;
	return tmp_begin(imp);
}

// Decide the system from the ROM header; -1 if it isn't a GB/GBC/GBA ROM.
static int detect_sys(const char *path, int hint)
{
	static const unsigned char gb_logo[4] = { 0xce, 0xed, 0x66, 0x66 };
	unsigned char h[0x150];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = pread(fd, h, sizeof h, 0);
	close(fd);
	if (hint == COZ_SYS_GBA)
		return n >= 0xc0 && h[0xb2] == 0x96 ? COZ_SYS_GBA : -1;
	if (n < 0x150 || memcmp(h + 0x104, gb_logo, 4))
		return -1;
	// CGB flag: 0x80 works on both (plays in colour), 0xc0 colour only
	return h[0x143] & 0x80 ? COZ_SYS_GBC : COZ_SYS_GB;
}

/* ---- No-Intro names --------------------------------------------------------- */

static int file_crc32(const char *path, uint32_t *out)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	uLong crc = crc32(0L, Z_NULL, 0);
	unsigned char buf[65536];
	ssize_t n;
	while ((n = read(fd, buf, sizeof buf)) > 0)
		crc = crc32(crc, buf, (uInt)n);
	close(fd);
	if (n < 0)
		return -EIO;
	*out = (uint32_t)crc;
	return 0;
}

// The canonical name of a ROM with this CRC, looked up in sys's list (and,
// for GB/GBC, the other one: dual-mode carts sit in either). 0 if found.
static int nointro_lookup(int sys, uint32_t crc, char *name, size_t n)
{
	const char *dir = getenv("COZ_NOINTRO") ? getenv("COZ_NOINTRO") : "/usr/share/cozisboy/nointro";
	int order[2] = {sys, sys == COZ_SYS_GB ? COZ_SYS_GBC : sys == COZ_SYS_GBC ? COZ_SYS_GB : -1};
	char want[9];
	snprintf(want, sizeof want, "%08X", crc);
	for (int k = 0; k < 2 && order[k] >= 0; k++) {
		char p[PATH_MAX], line[COZ_NAME_MAX + 16];
		snprintf(p, sizeof p, "%s/%s.txt", dir, SYS[order[k]].key);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		while (fgets(line, sizeof line, f)) {
			if (strncmp(line, want, 8) || line[8] != ' ')
				continue;
			line[strcspn(line, "\r\n")] = 0;
			fclose(f);
			if (!valid_name(line + 9))
				return -EINVAL;
			snprintf(name, n, "%s", line + 9);
			return 0;
		}
		fclose(f);
	}
	return -ENOENT;
}

int coz_game_identify(coz_game *g)
{
	char rom[PATH_MAX], name[COZ_NAME_MAX + 1];
	uint32_t crc;
	int r;
	if ((r = coz_game_rom_path(g, rom, sizeof rom)) || (r = file_crc32(rom, &crc)) ||
	    (r = nointro_lookup(g->sys, crc, name, sizeof name)))
		return r;
	if (!strcmp(name, g->name))
		return 0;
	r = coz_game_rename(g, name);
	return r ? r : 1;
}

int coz_import_commit(coz_import *imp, int replace, coz_game *out)
{
	int r = tmp_finish(imp);
	if (r)
		goto fail;
	int sys = detect_sys(imp->tmp, imp->sys);
	if (sys < 0) {
		r = -EINVAL;
		goto fail;
	}
	// a known dump: its proper name, whatever the file was called
	uint32_t crc;
	char known[COZ_NAME_MAX + 1];
	if (file_crc32(imp->tmp, &crc) == 0 && nointro_lookup(sys, crc, known, sizeof known) == 0)
		snprintf(imp->name, sizeof imp->name, "%s", known);
	char dir[PATH_MAX], rom[PATH_MAX], ext[8];
	if ((r = game_dir(sys, imp->name, dir, sizeof dir)))
		goto fail;
	if (access(dir, F_OK) == 0) {
		if (!replace) {
			r = -EEXIST;
			goto fail;
		}
		// Replace only the ROM: save, states and cover stay.
		if (find_rom(dir, ext) == 0) {
			snprintf(rom, sizeof rom, "%s/rom.%s", dir, ext);
			unlink(rom);
		}
	} else {
		if ((r = mkdirs(dir)))
			goto fail;
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/info", dir);
		FILE *f = fopen(p, "w");
		if (f) {
			fprintf(f, "added=%lld\n", (long long)time(NULL));
			fclose(f);
		}
	}
	snprintf(rom, sizeof rom, "%s/rom.%s", dir, SYS[sys].ext);
	if (rename(imp->tmp, rom)) {
		r = -errno;
		goto fail;
	}
	imp->tmp[0] = 0;
	fsync_dir(dir);
	if (out) {
		memset(out, 0, sizeof *out);
		out->sys = sys;
		snprintf(out->name, sizeof out->name, "%s", imp->name);
		coz_game_refresh(out);
	}
	return 0;
fail:
	coz_import_abort(imp);
	return r;
}

int coz_game_delete(const coz_game *g)
{
	char dir[PATH_MAX], trash[PATH_MAX];
	int r = game_dir(g->sys, g->name, dir, sizeof dir);
	if (r)
		return r;
	// Move out of the library first (atomic), then remove.
	snprintf(trash, sizeof trash, "%s/.tmp/delete-%s-%ld", coz_lib_root(), g->id, (long)getpid());
	char t[PATH_MAX];
	snprintf(t, sizeof t, "%s/.tmp", coz_lib_root());
	mkdirs(t);
	if (rename(dir, trash))
		return -errno;
	parent_of(dir, t, sizeof t);
	fsync_dir(t);
	return rm_tree(trash);
}

int coz_game_rename(coz_game *g, const char *new_name)
{
	if (!valid_name(new_name))
		return -EINVAL;
	char from[PATH_MAX], to[PATH_MAX];
	int r;
	if ((r = game_dir(g->sys, g->name, from, sizeof from)) ||
	    (r = game_dir(g->sys, new_name, to, sizeof to)))
		return r;
	if (!strcmp(from, to))
		return 0;
	if (access(to, F_OK) == 0)
		return -EEXIST;
	if (rename(from, to))
		return -errno;
	parent_of(to, from, sizeof from);
	fsync_dir(from);
	snprintf(g->name, sizeof g->name, "%s", new_name);
	return coz_game_refresh(g);
}

// ---------------------------------------------------------------- saves

int coz_save_import_begin(const coz_game *g, coz_save_import *imp)
{
	(void)g;
	memset(imp, 0, sizeof *imp);
	imp->fd = -1;
	return tmp_begin(imp);
}

int coz_save_slot_import_commit(const coz_game *g, int slot, coz_save_import *imp)
{
	char sav[PATH_MAX], bak[PATH_MAX];
	int r = tmp_finish(imp);
	if (r || (r = coz_save_slot_path(g, slot, sav, sizeof sav)) ||
	    (r = coz_save_slot_backup_path(g, slot, bak, sizeof bak)))
		goto fail;
	if (access(sav, F_OK) == 0 && rename(sav, bak)) {
		r = -errno;
		goto fail;
	}
	if (rename(imp->tmp, sav)) {
		r = -errno;
		goto fail;
	}
	imp->tmp[0] = 0;
	parent_of(sav, bak, sizeof bak);
	fsync_dir(bak);
	return 0;
fail:
	coz_import_abort(imp);
	return r;
}

int coz_save_slot_delete(const coz_game *g, int slot)
{
	char sav[PATH_MAX], bak[PATH_MAX];
	int r;
	if ((r = coz_save_slot_path(g, slot, sav, sizeof sav)) ||
	    (r = coz_save_slot_backup_path(g, slot, bak, sizeof bak)))
		return r;
	if (rename(sav, bak))
		return -errno;
	parent_of(sav, bak, sizeof bak);
	fsync_dir(bak);
	return 0;
}

int coz_save_slot_restore_backup(const coz_game *g, int slot)
{
	char sav[PATH_MAX], bak[PATH_MAX], tmp[PATH_MAX];
	int r;
	if ((r = coz_save_slot_path(g, slot, sav, sizeof sav)) ||
	    (r = coz_save_slot_backup_path(g, slot, bak, sizeof bak)))
		return r;
	if (access(bak, F_OK))
		return -ENOENT;
	game_file(g, "save.swap", tmp, sizeof tmp);
	int had_save = access(sav, F_OK) == 0;
	if (had_save && rename(sav, tmp))
		return -errno;
	if (rename(bak, sav))
		return -errno;
	if (had_save && rename(tmp, bak))
		return -errno;
	parent_of(sav, tmp, sizeof tmp);
	fsync_dir(tmp);
	return 0;
}

int coz_save_slot_remove(const coz_game *g, int slot)
{
	if (slot == 1)
		return -EINVAL;
	char sav[PATH_MAX], bak[PATH_MAX];
	int r;
	if ((r = coz_save_slot_path(g, slot, sav, sizeof sav)) ||
	    (r = coz_save_slot_backup_path(g, slot, bak, sizeof bak)))
		return r;
	int gone = 0;
	if (unlink(sav) == 0)
		gone = 1;
	else if (errno != ENOENT)
		return -errno;
	if (unlink(bak) == 0)
		gone = 1;
	else if (errno != ENOENT)
		return -errno;
	if (!gone)
		return -ENOENT;
	parent_of(sav, bak, sizeof bak);
	fsync_dir(bak);
	return 0;
}

int coz_save_slot_copy(const coz_game *g, int from, int to)
{
	char src[PATH_MAX];
	int r;
	if (from == to)
		return -EINVAL;
	if ((r = coz_save_slot_path(g, from, src, sizeof src)))
		return r;
	int in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -errno;
	coz_save_import imp;
	if ((r = coz_save_import_begin(g, &imp))) {
		close(in);
		return r;
	}
	char buf[16384];
	ssize_t n;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(imp.fd, buf, (size_t)n) != n) {
			n = -1;
			break;
		}
	r = n < 0 ? -EIO : 0;
	close(in);
	if (r) {
		coz_import_abort(&imp);
		return r;
	}
	return coz_save_slot_import_commit(g, to, &imp);
}

// rename a -> b and b -> a; either may be missing
static int swap_files(const coz_game *g, const char *a, const char *b)
{
	char tmp[PATH_MAX];
	game_file(g, "save.swap", tmp, sizeof tmp);
	int ha = access(a, F_OK) == 0, hb = access(b, F_OK) == 0;
	if (ha && rename(a, tmp))
		return -errno;
	if (hb && rename(b, a))
		return -errno;
	if (ha && rename(tmp, b))
		return -errno;
	return 0;
}

int coz_save_slot_swap(const coz_game *g, int a, int b)
{
	char sa[PATH_MAX], sb[PATH_MAX], ba[PATH_MAX], bb[PATH_MAX];
	int r;
	if (a == b)
		return -EINVAL;
	if ((r = coz_save_slot_path(g, a, sa, sizeof sa)) || (r = coz_save_slot_path(g, b, sb, sizeof sb)) ||
	    (r = coz_save_slot_backup_path(g, a, ba, sizeof ba)) ||
	    (r = coz_save_slot_backup_path(g, b, bb, sizeof bb)))
		return r;
	if ((r = swap_files(g, sa, sb)) || (r = swap_files(g, ba, bb)))
		return r;
	parent_of(sa, ba, sizeof ba);
	fsync_dir(ba);
	return 0;
}

int coz_saves_list(const coz_game *g, coz_save *out)
{
	int k = 0;
	for (int n = 1; n <= COZ_SAVE_SLOTS; n++) {
		char sav[PATH_MAX], bak[PATH_MAX];
		struct stat st, sb;
		int r;
		if ((r = coz_save_slot_path(g, n, sav, sizeof sav)) ||
		    (r = coz_save_slot_backup_path(g, n, bak, sizeof bak)))
			return r;
		int hs = stat(sav, &st) == 0, hb = stat(bak, &sb) == 0;
		if (!hs && !hb && n > 1)
			continue;
		out[k++] = (coz_save){.slot = n, .has_save = hs, .size = hs ? st.st_size : 0,
				      .time = hs ? st.st_mtime : 0, .has_backup = hb,
				      .backup_time = hb ? sb.st_mtime : 0};
	}
	return k;
}

int coz_save_slot_free(const coz_game *g)
{
	for (int n = 2; n <= COZ_SAVE_SLOTS; n++)
		if (!(g->save_slots & (1u << (n - 1))))
			return n;
	return -ENOSPC;
}

int coz_save_import_commit(const coz_game *g, coz_save_import *imp) { return coz_save_slot_import_commit(g, 1, imp); }
int coz_save_delete(const coz_game *g) { return coz_save_slot_delete(g, 1); }
int coz_save_restore_backup(const coz_game *g) { return coz_save_slot_restore_backup(g, 1); }

// ---------------------------------------------------------------- cover

int coz_cover_import_begin(const coz_game *g, coz_import *imp)
{
	return coz_save_import_begin(g, imp);
}

int coz_cover_import_commit(const coz_game *g, coz_import *imp)
{
	static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	unsigned char h[8] = { 0 };
	char cover[PATH_MAX];
	int r = tmp_finish(imp);
	if (r)
		goto fail;
	int fd = open(imp->tmp, O_RDONLY | O_CLOEXEC);
	if (fd < 0 || pread(fd, h, 8, 0) != 8 || memcmp(h, sig, 8)) {
		if (fd >= 0)
			close(fd);
		r = -EINVAL;
		goto fail;
	}
	close(fd);
	if ((r = coz_game_cover_path(g, cover, sizeof cover)))
		goto fail;
	if (rename(imp->tmp, cover)) {
		r = -errno;
		goto fail;
	}
	imp->tmp[0] = 0;
	return 0;
fail:
	coz_import_abort(imp);
	return r;
}

int coz_cover_load(const coz_game *g, int w, int h, uint32_t *out)
{
	char p[PATH_MAX];
	coz_game_cover_path(g, p, sizeof p);
	png_image img;
	memset(&img, 0, sizeof img);
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&img, p))
		return access(p, F_OK) ? -ENOENT : -EINVAL;
	img.format = PNG_FORMAT_RGB;
	size_t sw = img.width, sh = img.height;
	if (!sw || !sh || sw > 8192 || sh > 8192) {
		png_image_free(&img);
		return -EINVAL;
	}
	unsigned char *rgb = malloc(PNG_IMAGE_SIZE(img));
	if (!rgb) {
		png_image_free(&img);
		return -ENOMEM;
	}
	if (!png_image_finish_read(&img, NULL, rgb, 0, NULL)) {
		free(rgb);
		png_image_free(&img);
		return -EINVAL;
	}
	// fill w x h: scale so the image covers it, crop the overflow evenly
	double s = (double)w / sw > (double)h / sh ? (double)w / sw : (double)h / sh;
	double ox = (sw * s - w) / 2, oy = (sh * s - h) / 2;
	for (int y = 0; y < h; y++) {
		size_t sy = (size_t)((y + oy + 0.5) / s);
		if (sy >= sh)
			sy = sh - 1;
		for (int x = 0; x < w; x++) {
			size_t sx = (size_t)((x + ox + 0.5) / s);
			if (sx >= sw)
				sx = sw - 1;
			const unsigned char *px = rgb + (sy * sw + sx) * 3;
			out[(size_t)y * w + x] = (uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2];
		}
	}
	free(rgb);
	return 0;
}

int coz_cover_delete(const coz_game *g)
{
	char p[PATH_MAX];
	coz_game_cover_path(g, p, sizeof p);
	return unlink(p) ? -errno : 0;
}

// --------------------------------------------------------------- states

static int valid_state_id(const char *id)
{
	// YYYYMMDD-HHMMSS-N
	size_t l = strlen(id);
	if (l < 17 || l > 20)
		return 0;
	for (size_t i = 0; i < l; i++)
		if (!(isdigit((unsigned char)id[i]) || ((i == 8 || i == 15) && id[i] == '-')))
			return 0;
	return id[8] == '-' && id[15] == '-';
}

static time_t state_time(const char *id)
{
	struct tm tm = { 0 };
	if (sscanf(id, "%4d%2d%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
		   &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
		return 0;
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	return timegm(&tm);
}

static int cmp_states(const void *a, const void *b)
{
	const coz_state *x = a, *y = b;
	if (x->time != y->time)
		return x->time < y->time ? 1 : -1;
	return strcmp(y->id, x->id);
}

int coz_states_list(const coz_game *g, coz_state **out, int *n)
{
	char dir[PATH_MAX];
	*out = NULL;
	*n = 0;
	game_file(g, "states", dir, sizeof dir);
	DIR *d = opendir(dir);
	if (!d)
		return errno == ENOENT ? 0 : -errno;
	int cap = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		char *dot = strrchr(e->d_name, '.');
		if (!dot || strcmp(dot, ".state") || dot - e->d_name >= 24)
			continue;
		coz_state s = { 0 };
		memcpy(s.id, e->d_name, dot - e->d_name);
		if (!valid_state_id(s.id))
			continue;
		struct stat st;
		if (fstatat(dirfd(d), e->d_name, &st, 0))
			continue;
		s.size = st.st_size;
		s.time = state_time(s.id);
		char png[64];
		snprintf(png, sizeof png, "%s.png", s.id);
		s.has_thumb = faccessat(dirfd(d), png, F_OK, 0) == 0;
		if (*n == cap) {
			cap = cap ? cap * 2 : 16;
			coz_state *ns = realloc(*out, cap * sizeof *ns);
			if (!ns) {
				closedir(d);
				free(*out);
				*out = NULL;
				*n = 0;
				return -ENOMEM;
			}
			*out = ns;
		}
		(*out)[(*n)++] = s;
	}
	closedir(d);
	if (*n)
		qsort(*out, *n, sizeof **out, cmp_states);
	return 0;
}

int coz_state_path(const coz_game *g, const char *id, char *buf, size_t len)
{
	if (!valid_state_id(id))
		return -EINVAL;
	char f[64];
	snprintf(f, sizeof f, "states/%s.state", id);
	return game_file(g, f, buf, len);
}

int coz_state_thumb_path(const coz_game *g, const char *id, char *buf, size_t len)
{
	if (!valid_state_id(id))
		return -EINVAL;
	char f[64];
	snprintf(f, sizeof f, "states/%s.png", id);
	return game_file(g, f, buf, len);
}

static int write_file(const char *path, const void *data, size_t size)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	const char *p = data;
	while (size) {
		ssize_t n = write(fd, p, size);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int e = -errno;
			close(fd);
			unlink(tmp);
			return e;
		}
		p += n;
		size -= n;
	}
	if (fsync(fd) || close(fd) || rename(tmp, path)) {
		int e = -errno;
		unlink(tmp);
		return e;
	}
	return 0;
}

int coz_state_add(const coz_game *g, const void *data, size_t size,
		  const uint32_t *thumb, int w, int h, int stride, coz_state *out)
{
	char dir[PATH_MAX], p[PATH_MAX];
	game_file(g, "states", dir, sizeof dir);
	int r = mkdirs(dir);
	if (r)
		return r;
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm);
	coz_state s = { .time = now, .size = size };
	for (int k = 0; k < 100; k++) {
		snprintf(s.id, sizeof s.id, "%04d%02d%02d-%02d%02d%02d-%d", tm.tm_year + 1900,
			 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, k);
		snprintf(p, sizeof p, "%s/%s.state", dir, s.id);
		if (access(p, F_OK))
			break;
	}
	if (thumb && w > 0 && h > 0) {
		char png[PATH_MAX];
		snprintf(png, sizeof png, "%s/%s.png", dir, s.id);
		s.has_thumb = coz_png_write(png, thumb, w, h, stride) == 0;
	}
	if ((r = write_file(p, data, size)))
		return r;
	fsync_dir(dir);
	if (out)
		*out = s;
	return 0;
}

int coz_state_delete(const coz_game *g, const char *id)
{
	char p[PATH_MAX];
	int r = coz_state_path(g, id, p, sizeof p);
	if (r)
		return r;
	if (unlink(p))
		return -errno;
	coz_state_thumb_path(g, id, p, sizeof p);
	unlink(p);
	return 0;
}

// ------------------------------------------------------------------ PNG

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const unsigned char *b, size_t n)
{
	if (!crc_table[1])
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t v = i;
			for (int k = 0; k < 8; k++)
				v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
			crc_table[i] = v;
		}
	c ^= 0xffffffffu;
	while (n--)
		c = crc_table[(c ^ *b++) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

static int png_chunk(FILE *f, const char *type, const unsigned char *data, size_t n)
{
	unsigned char hdr[8];
	put32(hdr, n);
	memcpy(hdr + 4, type, 4);
	uint32_t c = crc32_update(0, hdr + 4, 4);
	c = crc32_update(c, data, n);
	unsigned char crc[4];
	put32(crc, c);
	return fwrite(hdr, 8, 1, f) != 1 || (n && fwrite(data, n, 1, f) != 1) ||
	       fwrite(crc, 4, 1, f) != 1;
}

// RGB, filter 0, zlib with stored (uncompressed) deflate blocks.
int coz_png_write(const char *path, const uint32_t *px, int w, int h, int stride)
{
	size_t raw_len = (size_t)h * (1 + 3 * (size_t)w);
	size_t nblocks = (raw_len + 65534) / 65535;
	size_t z_len = 2 + raw_len + 5 * nblocks + 4;
	unsigned char *raw = malloc(raw_len), *z = malloc(z_len);
	if (!raw || !z) {
		free(raw);
		free(z);
		return -ENOMEM;
	}
	unsigned char *q = raw;
	for (int y = 0; y < h; y++) {
		*q++ = 0;
		const uint32_t *row = px + (size_t)y * stride;
		for (int x = 0; x < w; x++) {
			*q++ = row[x] >> 16;
			*q++ = row[x] >> 8;
			*q++ = row[x];
		}
	}
	unsigned char *o = z;
	*o++ = 0x78;
	*o++ = 0x01;
	uint32_t a = 1, b = 0;
	for (size_t off = 0; off < raw_len;) {
		size_t n = raw_len - off > 65535 ? 65535 : raw_len - off;
		*o++ = off + n == raw_len;	// BFINAL, BTYPE=00
		*o++ = n & 0xff;
		*o++ = n >> 8;
		*o++ = ~n & 0xff;
		*o++ = (~n >> 8) & 0xff;
		memcpy(o, raw + off, n);
		for (size_t i = 0; i < n; i++) {
			a = (a + raw[off + i]) % 65521;
			b = (b + a) % 65521;
		}
		o += n;
		off += n;
	}
	put32(o, (b << 16) | a);
	o += 4;
	free(raw);

	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	int err = !f;
	if (f) {
		static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
		unsigned char ihdr[13];
		put32(ihdr, w);
		put32(ihdr + 4, h);
		ihdr[8] = 8;	// bit depth
		ihdr[9] = 2;	// RGB
		ihdr[10] = ihdr[11] = ihdr[12] = 0;
		err = fwrite(sig, 8, 1, f) != 1 || png_chunk(f, "IHDR", ihdr, 13) ||
		      png_chunk(f, "IDAT", z, o - z) || png_chunk(f, "IEND", NULL, 0);
		err |= fflush(f) || fsync(fileno(f));
		err |= fclose(f) != 0;
	}
	free(z);
	if (err || rename(tmp, path)) {
		unlink(tmp);
		return -EIO;
	}
	return 0;
}
