/**
 * @file luaz_fs_shell.c
 * @brief Shell commands for managing Lua scripts on the LittleFS filesystem.
 *
 * Provides the `lua_fs` shell command group with subcommands for listing,
 * reading, writing, deleting, running scripts, and showing FS statistics.
 * Enabled via CONFIG_LUA_FS_SHELL.
 */

#ifdef CONFIG_LUA_FS_SHELL

#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <luaz_utils.h>
#include <luaz_fs.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>

#define MOUNT_POINT CONFIG_LUA_FS_MOUNT_POINT
#define MAX_PATH    64

/** @brief End-of-transmission (Ctrl+D) — exits multi-line input. */
#define EOT 0x04
/** @brief Backspace character. */
#define BS  0x08
/** @brief Delete character. */
#define DEL 0x7F

/**
 * @brief Build an absolute path from a filename.
 *
 * Prepends the mount point if the name does not start with '/'.
 */
static int build_shell_path(char *buf, size_t buflen, const char *name)
{
	int ret;

	if (name[0] == '/') {
		if (strlen(name) >= buflen) {
			return -ENAMETOOLONG;
		}
		strcpy(buf, name);
	} else {
		ret = snprintf(buf, buflen, "%s/%s", MOUNT_POINT, name);
		if (ret < 0 || (size_t)ret >= buflen) {
			return -ENAMETOOLONG;
		}
	}
	return 0;
}

/** @brief Shell command: lua_fs list — list files in the mount point. */
static int cmd_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct fs_dir_t dir;

	fs_dir_t_init(&dir);

	int rc = fs_opendir(&dir, MOUNT_POINT);

	if (rc < 0) {
		shell_error(sh, "Cannot open %s: %d", MOUNT_POINT, rc);
		return rc;
	}

	shell_print(sh, "%-32s %s", "Name", "Size");
	shell_print(sh, "%-32s %s", "----", "----");

	while (true) {
		struct fs_dirent entry;

		rc = fs_readdir(&dir, &entry);
		if (rc < 0 || entry.name[0] == '\0') {
			break;
		}

		if (entry.type == FS_DIR_ENTRY_DIR) {
			shell_print(sh, "%-32s <DIR>", entry.name);
		} else {
			shell_print(sh, "%-32s %zu", entry.name, entry.size);
		}
	}

	fs_closedir(&dir);
	return 0;
}

/** @brief Shell command: lua_fs cat <name> — print file contents. */
static int cmd_cat(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: lua_fs cat <filename>");
		return -EINVAL;
	}

	char path[MAX_PATH];
	int rc = build_shell_path(path, sizeof(path), argv[1]);

	if (rc < 0) {
		shell_error(sh, "Path too long");
		return rc;
	}

	struct fs_file_t file;

	fs_file_t_init(&file);

	rc = fs_open(&file, path, FS_O_READ);
	if (rc < 0) {
		shell_error(sh, "Cannot open %s: %d", path, rc);
		return rc;
	}

	char buf[128];

	while (true) {
		ssize_t n = fs_read(&file, buf, sizeof(buf) - 1);

		if (n <= 0) {
			break;
		}
		buf[n] = '\0';
		shell_fprintf(sh, SHELL_NORMAL, "%s", buf);
	}

	shell_print(sh, "");
	fs_close(&file);
	return 0;
}

/**
 * @brief Read one line from the shell transport (for multi-line write input).
 *
 * Uses the same approach as lua_repl.c shell_getline.
 */
static int lua_fs_shell_readline(const struct shell *sh, char *buf, size_t len, bool *eof)
{
	const struct shell_transport_api *sh_api = sh->iface->api;

	(void)memset(buf, 0, len);
	*eof = false;

	for (size_t i = 0; i < (len - 1);) {
		char c;
		size_t cnt = 0;

		WAIT_FOR(cnt > 0, UINT32_MAX, (sh_api->read(sh->iface, &c, sizeof(c), &cnt)));

		if (c == BS || c == DEL) {
			if (i > 0) {
				i--;
				buf[i] = '\0';
				shell_fprintf(sh, SHELL_NORMAL, "\b \b");
			}
			continue;
		}

		sh_api->write(sh->iface, &c, sizeof(c), &cnt);

		if (c == EOT) {
			*eof = true;
			return (int)i;
		}

		if (c == '\n' || c == '\r') {
			return (int)i;
		}

		buf[i] = c;
		i++;
	}

	return (int)(len - 1);
}

/** @brief Shell command: lua_fs write <name> — write multi-line input to file. */
static int cmd_write(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: lua_fs write <filename>");
		return -EINVAL;
	}

	char path[MAX_PATH];
	int rc = build_shell_path(path, sizeof(path), argv[1]);

	if (rc < 0) {
		shell_error(sh, "Path too long");
		return rc;
	}

	struct fs_file_t file;

	fs_file_t_init(&file);

	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (rc < 0) {
		shell_error(sh, "Cannot open %s: %d", path, rc);
		return rc;
	}

	rc = fs_truncate(&file, 0);
	if (rc < 0) {
		shell_error(sh, "Cannot truncate %s: %d", path, rc);
		fs_close(&file);
		return rc;
	}

	shell_print(sh, "Enter script (empty line with '.' to finish, Ctrl+D to cancel):");

	char line[256];

	while (true) {
		shell_fprintf(sh, SHELL_NORMAL, "> ");
		bool eof = false;
		int n = lua_fs_shell_readline(sh, line, sizeof(line), &eof);

		shell_print(sh, "");

		if (eof) {
			shell_print(sh, "Cancelled.");
			fs_close(&file);
			fs_unlink(path);
			return 0;
		}

		/* Single dot on empty line = end of input */
		if (n == 1 && line[0] == '.') {
			break;
		}

		if (n > 0) {
			fs_write(&file, line, n);
		}
		fs_write(&file, "\n", 1);
	}

	fs_close(&file);
	shell_print(sh, "Written to %s", path);
	return 0;
}

/** @brief Shell command: lua_fs delete <name> — delete a file. */
static int cmd_delete(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: lua_fs delete <filename>");
		return -EINVAL;
	}

	char path[MAX_PATH];
	int rc = build_shell_path(path, sizeof(path), argv[1]);

	if (rc < 0) {
		shell_error(sh, "Path too long");
		return rc;
	}

	rc = fs_unlink(path);
	if (rc < 0) {
		shell_error(sh, "Cannot delete %s: %d", path, rc);
		return rc;
	}

	shell_print(sh, "Deleted %s", path);
	return 0;
}

/** @brief Shell command: lua_fs run <name> — execute a script in a temporary Lua state. */
/* A k_heap, so heap viewers such as zview find it; Lua allocates from its sys_heap. */
K_HEAP_DEFINE(lua_fs_run_heap, CONFIG_LUA_THREAD_HEAP_SIZE);

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: lua_fs run <filename>");
		return -EINVAL;
	}

	/* Start each run from an empty heap. */
	sys_heap_init(&lua_fs_run_heap.heap, lua_fs_run_heap.heap.init_mem,
		      lua_fs_run_heap.heap.init_bytes);

	lua_State *L = lua_newstate(lua_zephyr_allocator, &lua_fs_run_heap.heap, 0);

	if (L == NULL) {
		shell_error(sh, "Failed to create Lua state");
		return -ENOMEM;
	}

	luaz_openlibs(L);

	int rc = lua_fs_dofile(L, argv[1]);

	if (rc != 0) {
		if (lua_isstring(L, -1)) {
			shell_error(sh, "Error: %s", lua_tostring(L, -1));
		} else {
			shell_error(sh, "Error: %d", rc);
		}
	}

	lua_close(L);
	return rc;
}

/** @brief Shell command: lua_fs stat — show filesystem statistics. */
static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct fs_statvfs stat;
	int rc = fs_statvfs(MOUNT_POINT, &stat);

	if (rc < 0) {
		shell_error(sh, "fs_statvfs failed: %d", rc);
		return rc;
	}

	shell_print(sh, "Filesystem: %s", MOUNT_POINT);
	shell_print(sh, "  Block size:    %lu", stat.f_bsize);
	shell_print(sh, "  Total blocks:  %lu", stat.f_blocks);
	shell_print(sh, "  Free blocks:   %lu", stat.f_bfree);

	return 0;
}

/* clang-format off */
SHELL_STATIC_SUBCMD_SET_CREATE(lua_fs_cmds,
	SHELL_CMD(list,   NULL, "List files on the Lua filesystem",       cmd_list),
	SHELL_CMD(cat,    NULL, "Print file contents: cat <filename>",    cmd_cat),
	SHELL_CMD(write,  NULL, "Write a script: write <filename>",       cmd_write),
	SHELL_CMD(delete, NULL, "Delete a file: delete <filename>",       cmd_delete),
	SHELL_CMD(run,    NULL, "Execute a script: run <filename>",       cmd_run),
	SHELL_CMD(stat,   NULL, "Show filesystem statistics",             cmd_stat),
	SHELL_SUBCMD_SET_END
);
/* clang-format on */

SHELL_CMD_REGISTER(lua_fs, &lua_fs_cmds, "Lua filesystem commands", NULL);

#endif /* CONFIG_LUA_FS_SHELL */
