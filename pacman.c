/* vi: set sw=4 ts=4: */
/*
 * busybox_pacman - A lightweight pacman implementation for BusyBox
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */

//config:config PACMAN
//config:	bool "pacman (11 kb)"
//config:	default y
//config:	select TAR
//config:	select WGET
//config:	help
//config:	  pacman is a package manager for Arch Linux and its derivatives.
//config:	  This implementation is a lightweight frontend for BusyBox.

//applet:IF_PACMAN(APPLET(pacman, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_PACMAN) += pacman.o

//usage:#define pacman_trivial_usage
//usage:       "[-SsyuRi] [--reinstall] [--rescue-install] [--verify] [PACKAGE...]"
//usage:#define pacman_full_usage "\n\n"
//usage:       "Arch Linux package manager\n"
//usage:     "\nOptions:"
//usage:     "\n    -S                  Synchronize/Install packages"
//usage:     "\n    -s                  Search for packages"
//usage:     "\n    -y                  Refresh package databases"
//usage:     "\n    -u                  Upgrade installed packages"
//usage:     "\n    -R                  Remove packages"
//usage:     "\n    -i                  Show package information"
//usage:     "\n    --reinstall         Reinstall packages"
//usage:     "\n    --rescue-install    Rescue installation"
//usage:     "\n    --verify            Verify package integrity"
//usage:     "\n"
//usage:     "\nCommands:"
//usage:     "\n    md5check            Verify MD5 sums of installed files"

#include "libbb.h"
#include "bb_archive.h"
#include "common_bufsiz.h"
#include <sys/utsname.h>
#include <ctype.h>

/* Terminal Colors */
#define CLR_GREEN  "\033[1;32m"
#define CLR_RED    "\033[1;31m"
#define CLR_BOLD   "\033[1m"
#define CLR_RESET  "\033[0m"

/* ==========================================================================
 * 1. DATA STRUCTURES & MULTI-PURPOSE ENUMS
 * ========================================================================== */

typedef enum {
    PACMAN_STATE_INIT = 0,
    PACMAN_STATE_SYNC_DB,      /* Fetch/Update core.db, extra.db, etc. */
    PACMAN_STATE_PARSE_DB,     /* Read tar.gz DB looking for pkgname-version */
    PACMAN_STATE_SEARCH,       /* Search available packages */
    PACMAN_STATE_INFO,         /* Show package information */
    PACMAN_STATE_RESOLVE_DEPS, /* Build the dependency graph */
    PACMAN_STATE_DOWNLOAD,     /* Fetch .pkg.tar.zst files */
    PACMAN_STATE_COMMIT,       /* Extract and run ALPM hooks (pre/post install) */
    PACMAN_STATE_REMOVE,       /* Remove packages */
    PACMAN_STATE_VERIFY,       /* Verify package integrity */
    PACMAN_STATE_CLEANUP,
    PACMAN_STATE_FATAL
} pacman_state_t;

typedef struct {
    char *name;
    llist_t *servers;
} pacman_repo_t;

typedef struct pkg_info_t {
    char *name;
    char *version;
    char *desc;
    char *filename;
    llist_t *depends;
    llist_t *provides;
    llist_t *groups;
    unsigned long csize;
    unsigned long isize;
    char *repo_name;
    int state; /* 0: available, 1: target, 2: resolved */
    char *url;
    char *license;
    char *md5sum;
    char *sha256sum;
} pkg_info_t;

struct globals {
    pacman_state_t current_state;
    unsigned opts;
    llist_t *target_pkgs;
    llist_t *resolved_pkgs;

    /* Configuration */
    char *dbpath;
    char *cachedir;
    char *root;
    char *arch;
    llist_t *repos;

    /* DB processing */
    llist_t *available_pkgs;
    llist_t *installed_pkgs;
    char *current_repo_name;
    char bb_path[1024];
} FIX_ALIASING;
#define G (*(struct globals*)bb_common_bufsiz1)

#define OPT_S         (1 << 0)
#define OPT_s         (1 << 1)
#define OPT_y         (1 << 2)
#define OPT_u         (1 << 3)
#define OPT_R         (1 << 4)
#define OPT_i         (1 << 5)
#define OPT_reinstall (1 << 6)
#define OPT_rescue    (1 << 7)
#define OPT_verify    (1 << 8)
#define OPT_md5check  (1 << 9)

/* ==========================================================================
 * 2. UTILS & PARSERS
 * ========================================================================== */

static int llist_count(llist_t *list)
{
    int count = 0;
    while (list) {
        count++;
        list = list->link;
    }
    return count;
}

static char* string_replace(const char *src, const char *sub, const char *repl)
{
    int count = count_strstr(src, sub);
    return xmalloc_substitute_string(src, count, sub, repl);
}

static char* get_arch(void)
{
    struct utsname uts;
    uname(&uts);
    if (strcmp(uts.machine, "x86_64") == 0) return xstrdup("x86_64");
    if (strcmp(uts.machine, "i686") == 0) return xstrdup("i686");
    return xstrdup(uts.machine);
}

static void get_bb_exe(char *buf, size_t size)
{
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len == -1) strcpy(buf, "busybox");
    else buf[len] = '\0';
}

static pacman_repo_t* add_repo(const char *name)
{
    pacman_repo_t *repo = xzalloc(sizeof(pacman_repo_t));
    repo->name = xstrdup(name);
    llist_add_to_end(&G.repos, repo);
    return repo;
}

static void load_pacman_conf(const char *path)
{
    parser_t *p = config_open(path);
    char *tokens[3];
    pacman_repo_t *current_repo = NULL;

    if (!p) {
        bb_error_msg_and_die("pacman.conf not found at %s. Please ensure /etc/pacman.conf exists or set PACMAN_CONF environment variable.", path);
    }

    while (config_read(p, tokens, 3, 1, "# \t=", PARSE_NORMAL)) {
        if (tokens[0][0] == '[' && tokens[0][strlen(tokens[0])-1] == ']') {
            char *repo_name = xstrndup(tokens[0] + 1, strlen(tokens[0]) - 2);
            if (strcasecmp(repo_name, "options") != 0) {
                current_repo = add_repo(repo_name);
            } else {
                current_repo = NULL;
            }
            free(repo_name);
            continue;
        }

        if (strcasecmp(tokens[0], "DBPath") == 0 && tokens[1]) {
            G.dbpath = xstrdup(tokens[1]);
        } else if (strcasecmp(tokens[0], "CacheDir") == 0 && tokens[1]) {
            G.cachedir = xstrdup(tokens[1]);
        } else if (strcasecmp(tokens[0], "RootDir") == 0 && tokens[1]) {
            G.root = xstrdup(tokens[1]);
        } else if (strcasecmp(tokens[0], "Architecture") == 0 && tokens[1]) {
            if (strcmp(tokens[1], "auto") == 0)
                G.arch = get_arch();
            else
                G.arch = xstrdup(tokens[1]);
        } else if (current_repo && strcasecmp(tokens[0], "Server") == 0 && tokens[1]) {
            llist_add_to_end(&current_repo->servers, xstrdup(tokens[1]));
        } else if (current_repo && strcasecmp(tokens[0], "Include") == 0 && tokens[1]) {
            parser_t *sub = config_open(tokens[1]);
            char *sub_tok[3];
            if (sub) {
                while (config_read(sub, sub_tok, 3, 1, "# \t=", PARSE_NORMAL)) {
                    if (strcasecmp(sub_tok[0], "Server") == 0 && sub_tok[1]) {
                        llist_add_to_end(&current_repo->servers, xstrdup(sub_tok[1]));
                    }
                }
                config_close(sub);
            }
        }
    }
    config_close(p);

    if (!G.dbpath) G.dbpath = xstrdup("/var/lib/pacman");
    if (!G.cachedir) G.cachedir = xstrdup("/var/cache/pacman/pkg");
    if (!G.root) G.root = xstrdup("/");
    if (!G.arch) G.arch = get_arch();
}

static char* substitute_vars(const char *url, const char *repo)
{
    char *res = xstrdup(url);
    char *tmp;
    tmp = string_replace(res, "$repo", repo);
    free(res);
    res = tmp;
    tmp = string_replace(res, "$arch", G.arch);
    free(res);
    return tmp;
}

/* Version comparison logic */
static int order(char c)
{
    if (isdigit(c)) return 0;
    if (isalpha(c)) return (unsigned char)c;
    if (c == '~') return -1;
    if (c) return (unsigned char)c + 256;
    return 0;
}

static int compare_version_part(const char *v1, const char *v2)
{
    while (*v1 || *v2) {
        int first_diff = 0;
        while ((*v1 && !isdigit(*v1)) || (*v2 && !isdigit(*v2))) {
            int o1 = order(*v1);
            int o2 = order(*v2);
            if (o1 != o2) return o1 - o2;
            if (*v1) v1++;
            if (*v2) v2++;
        }
        while (*v1 == '0') v1++;
        while (*v2 == '0') v2++;
        while (isdigit(*v1) && isdigit(*v2)) {
            if (!first_diff) first_diff = *v1 - *v2;
            v1++; v2++;
        }
        if (isdigit(*v1)) return 1;
        if (isdigit(*v2)) return -1;
        if (first_diff) return first_diff;
    }
    return 0;
}

static int compare_versions(const char *v1, const char *v2)
{
    const char *e1, *e2;
    long epoch1 = 0, epoch2 = 0;
    const char *u1, *u2;
    const char *r1, *r2;

    if (!v1 || !v2) return v1 ? 1 : (v2 ? -1 : 0);

    e1 = strchr(v1, ':');
    if (e1) {
        epoch1 = strtol(v1, NULL, 10);
        u1 = e1 + 1;
    } else {
        u1 = v1;
    }

    e2 = strchr(v2, ':');
    if (e2) {
        epoch2 = strtol(v2, NULL, 10);
        u2 = e2 + 1;
    } else {
        u2 = v2;
    }

    if (epoch1 != epoch2) return (epoch1 > epoch2) ? 1 : -1;

    r1 = strrchr(u1, '-');
    r2 = strrchr(u2, '-');

    if (r1 && r2) {
        char *up1 = xstrndup(u1, r1 - u1);
        char *up2 = xstrndup(u2, r2 - u2);
        int res = compare_version_part(up1, up2);
        free(up1); free(up2);
        if (res) return res;
        return compare_version_part(r1 + 1, r2 + 1);
    } else if (r1) {
        char *up1 = xstrndup(u1, r1 - u1);
        int res = compare_version_part(up1, u2);
        free(up1);
        if (res) return res;
        return compare_version_part(r1 + 1, "");
    } else if (r2) {
        char *up2 = xstrndup(u2, r2 - u2);
        int res = compare_version_part(u1, up2);
        free(up2);
        if (res) return res;
        return compare_version_part("", r2 + 1);
    }
    return compare_version_part(u1, u2);
}

static void parse_alpm_metadata_block(pkg_info_t *pkg, const char *block_data)
{
    char *data = xstrdup(block_data);
    char *line = data;
    char *next;
    char *current_key = NULL;

    while (line && *line) {
        next = strchr(line, '\n');
        if (next) *next = '\0';

        if (line[0] == '%') {
            size_t len = strlen(line);
            if (len > 2 && line[len-1] == '%') {
                free(current_key);
                current_key = xstrndup(line + 1, len - 2);
            }
        } else if (current_key && line[0] != '\0') {
            if (strcmp(current_key, "NAME") == 0) {
                free(pkg->name);
                pkg->name = xstrdup(line);
            } else if (strcmp(current_key, "VERSION") == 0) {
                free(pkg->version);
                pkg->version = xstrdup(line);
            } else if (strcmp(current_key, "FILENAME") == 0) {
                free(pkg->filename);
                pkg->filename = xstrdup(line);
            } else if (strcmp(current_key, "DESC") == 0) {
                free(pkg->desc);
                pkg->desc = xstrdup(line);
            } else if (strcmp(current_key, "DEPENDS") == 0) {
                llist_add_to(&pkg->depends, xstrdup(line));
            } else if (strcmp(current_key, "PROVIDES") == 0) {
                llist_add_to(&pkg->provides, xstrdup(line));
            } else if (strcmp(current_key, "GROUPS") == 0) {
                llist_add_to(&pkg->groups, xstrdup(line));
            } else if (strcmp(current_key, "CSIZE") == 0) {
                pkg->csize = atol(line);
            } else if (strcmp(current_key, "ISIZE") == 0) {
                pkg->isize = atol(line);
            } else if (strcmp(current_key, "URL") == 0) {
                free(pkg->url);
                pkg->url = xstrdup(line);
            } else if (strcmp(current_key, "LICENSE") == 0) {
                free(pkg->license);
                pkg->license = xstrdup(line);
            } else if (strcmp(current_key, "MD5SUM") == 0) {
                free(pkg->md5sum);
                pkg->md5sum = xstrdup(line);
            } else if (strcmp(current_key, "SHA256SUM") == 0) {
                free(pkg->sha256sum);
                pkg->sha256sum = xstrdup(line);
            }
        }

        if (next) line = next + 1;
        else line = NULL;
    }
    free(current_key);
    free(data);
}

static void load_local_db(void)
{
    char *local_path = xasprintf("%s/local", G.dbpath);
    DIR *dir = opendir(local_path);
    struct dirent *entry;

    if (!dir) {
        free(local_path);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char *desc_file;
        if (entry->d_name[0] == '.') continue;

        desc_file = xasprintf("%s/%s/desc", local_path, entry->d_name);
        if (access(desc_file, R_OK) == 0) {
            char *buf;
            struct stat st;
            int fd = open(desc_file, O_RDONLY);
            if (fd >= 0) {
                pkg_info_t *pkg;
                fstat(fd, &st);
                buf = xmalloc(st.st_size + 1);
                full_read(fd, buf, st.st_size);
                buf[st.st_size] = '\0';
                close(fd);

                pkg = xzalloc(sizeof(pkg_info_t));
                pkg->repo_name = xstrdup("local");
                parse_alpm_metadata_block(pkg, buf);
                llist_add_to(&G.installed_pkgs, pkg);
                free(buf);
            }
        }
        free(desc_file);
    }
    closedir(dir);
    free(local_path);
}

/* ==========================================================================
 * 3. ARCHIVE HANDLING
 * ========================================================================== */

static void FAST_FUNC alpm_db_action_data(archive_handle_t *handle)
{
    if (strstr(handle->file_header->name, "/desc")) {
        char *buf;
        pkg_info_t *pkg;

        buf = xmalloc(handle->file_header->size + 1);
        full_read(handle->src_fd, buf, handle->file_header->size);
        buf[handle->file_header->size] = '\0';

        pkg = xzalloc(sizeof(pkg_info_t));
        pkg->repo_name = xstrdup(G.current_repo_name);
        parse_alpm_metadata_block(pkg, buf);
        llist_add_to(&G.available_pkgs, pkg);

        free(buf);
    } else {
        data_skip(handle);
    }
}

/* ==========================================================================
 * 4. FSM STATE HANDLERS
 * ========================================================================== */

static int download_file(const char *url, const char *dest)
{
    char *cmd;
    int rc;

    /* 1. Try system wget silently */
    cmd = xasprintf("wget -q -O %s \"%s\" 2>/dev/null", dest, url);
    rc = system(cmd);
    free(cmd);

    if (rc == 0) return 0; /* System tool succeeded! */

    /* 2. Fallback using our exact binary path */
    cmd = xasprintf("%s wget -q -O %s \"%s\"", G.bb_path, dest, url);
    rc = system(cmd);
    free(cmd);

    if (rc != 0) {
        bb_error_msg("Both system and internal wget failed to download %s", url);
    }
    return rc;
}

static int unzstd_file(const char *src, const char *dest)
{
    char *cmd;
    int rc;

    /* 1. Try system unzstd */
    cmd = xasprintf("unzstd -c \"%s\" > \"%s\" 2>/dev/null", src, dest);
    rc = system(cmd);
    free(cmd);
    if (rc == 0) return 0;

    /* 2. Try system zstd -dc */
    cmd = xasprintf("zstd -dc \"%s\" > \"%s\" 2>/dev/null", src, dest);
    rc = system(cmd);
    free(cmd);
    if (rc == 0) return 0;

    /* 3. Fallback to internal unzstd applet */
    cmd = xasprintf("%s unzstd -c \"%s\" > \"%s\" 2>/dev/null", G.bb_path, src, dest);
    rc = system(cmd);
    free(cmd);
    if (rc == 0) return 0;

    /* 4. Fallback to internal zstd applet */
    cmd = xasprintf("%s zstd -dc \"%s\" > \"%s\" 2>/dev/null", G.bb_path, src, dest);
    rc = system(cmd);
    free(cmd);
    if (rc == 0) return 0;

    /* 5. ULTIMATE FALLBACK: Use internal libbb decompression (Seamless support)
     * This works if the library has Zstd support even if the applet is disabled. */
    {
        int src_fd = open_zipped(src, 0);
        if (src_fd >= 0) {
            int dest_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (dest_fd >= 0) {
                bb_copyfd_eof(src_fd, dest_fd);
                close(dest_fd);
                close(src_fd);
                return 0;
            }
            close(src_fd);
        }
    }

    unlink(dest);
    bb_error_msg("Both system and internal unzstd/zstd failed for %s", src);
    return -1;
}

static int tar_file(const char *dest_dir, const char *tarball, const char *options, const char *files)
{
    char *cmd;
    int rc;

    /* 1. Try system tar silently. Use -a for auto-decompression fallback. */
    cmd = xasprintf("tar -C \"%s\" %s -xaf \"%s\" %s 2>/dev/null",
                    dest_dir, options ? options : "", tarball, files ? files : "");
    rc = system(cmd);
    free(cmd);

    if (rc == 0) return 0;

    /* 2. Fallback to internal tar */
    cmd = xasprintf("%s tar -C \"%s\" %s -xaf \"%s\" %s",
                    G.bb_path, dest_dir, options ? options : "", tarball, files ? files : "");
    rc = system(cmd);
    free(cmd);

    if (rc != 0) {
        bb_error_msg("Both system and internal tar failed to extract %s", tarball);
    }
    return rc;
}

static int tar_list(const char *tarball, const char *dest_file, const char *options, const char *files)
{
    char *cmd;
    int rc;

    /* 1. Try system tar silently */
    cmd = xasprintf("tar %s -tf \"%s\" %s >> \"%s\" 2>/dev/null",
                    options ? options : "", tarball, files ? files : "", dest_file);
    rc = system(cmd);
    free(cmd);

    if (rc == 0) return 0;

    /* 2. Fallback to internal tar */
    cmd = xasprintf("%s tar %s -tf \"%s\" %s >> \"%s\"",
                    G.bb_path, options ? options : "", tarball, files ? files : "", dest_file);
    rc = system(cmd);
    free(cmd);

    if (rc != 0) {
        bb_error_msg("Both system and internal tar failed to list %s", tarball);
    }
    return rc;
}

static pacman_state_t do_sync_db(void)
{
    llist_t *curr_repo = G.repos;
    char *sync_path = xasprintf("%s/sync", G.dbpath);

    printf(":: %sSynchronizing package databases...%s\n", CLR_BOLD, CLR_RESET);
    bb_make_directory(sync_path, 0755, FILEUTILS_RECUR);

    while (curr_repo) {
        pacman_repo_t *repo = (pacman_repo_t*)curr_repo->data;
        if (repo->servers) {
            char *url = substitute_vars((char*)repo->servers->data, repo->name);
            char *db_url = xasprintf("%s/%s.db", url, repo->name);
            char *dest = xasprintf("%s/%s.db", sync_path, repo->name);

            printf(" %s downloading...\n", repo->name);
            fflush(stdout);
            if (download_file(db_url, dest) != 0) {
                printf("error: failed to update %s (download library error)\n", repo->name);
            }

            free(dest);
            free(db_url);
            free(url);
        }
        curr_repo = curr_repo->link;
    }
    free(sync_path);
    return PACMAN_STATE_PARSE_DB;
}

static pacman_state_t do_parse_db(void)
{
    llist_t *curr_repo = G.repos;
    char *sync_path = xasprintf("%s/sync", G.dbpath);

    while (curr_repo) {
        pacman_repo_t *repo = (pacman_repo_t*)curr_repo->data;
        char *db_file = xasprintf("%s/%s.db", sync_path, repo->name);

        if (access(db_file, R_OK) == 0) {
            archive_handle_t *handle;

            G.current_repo_name = xstrdup(repo->name);
            handle = init_handle();
            handle->action_data = alpm_db_action_data;
            handle->filter = filter_accept_all;
            handle->src_fd = open_zipped(db_file, 0);

            if (handle->src_fd >= 0) {
                while (get_header_tar(handle) == EXIT_SUCCESS)
                    continue;
                close(handle->src_fd);
            }
            free(handle);
            free(G.current_repo_name);
            G.current_repo_name = NULL;
        }
        free(db_file);
        curr_repo = curr_repo->link;
    }
    free(sync_path);

    /* Reverse the list once in O(N) time so order is preserved */
    G.available_pkgs = llist_rev(G.available_pkgs);

    if (G.opts & OPT_s)
        return PACMAN_STATE_SEARCH;

    return PACMAN_STATE_RESOLVE_DEPS;
}

static pacman_state_t do_search(void)
{
    llist_t *curr_pkg;
    llist_t *curr_target;

    if (!G.target_pkgs) return PACMAN_STATE_CLEANUP;

    curr_pkg = G.available_pkgs;
    while (curr_pkg) {
        pkg_info_t *pkg = (pkg_info_t*)curr_pkg->data;
        curr_target = G.target_pkgs;
        while (curr_target) {
            char *pattern = (char*)curr_target->data;
            if (strstr(pkg->name, pattern) || (pkg->desc && strstr(pkg->desc, pattern))) {
                printf("%s/%s %s%s%s\n", pkg->repo_name, pkg->name, CLR_GREEN, pkg->version, CLR_RESET);
                if (pkg->desc) printf("    %s\n", pkg->desc);
                break;
            }
            curr_target = curr_target->link;
        }
        curr_pkg = curr_pkg->link;
    }

    return PACMAN_STATE_CLEANUP;
}

static pkg_info_t* find_package(const char *name)
{
    llist_t *curr;

    /* Pass 1: Exact name match (Respects repo priority via list order) */
    curr = G.available_pkgs;
    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        if (strcmp(pkg->name, name) == 0) return pkg;
        curr = curr->link;
    }

    /* Pass 2: Provides match (capabilities/virtual packages) */
    curr = G.available_pkgs;
    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        llist_t *prov = pkg->provides;
        while (prov) {
            char *pname = (char*)prov->data;
            if (strcmp(pname, name) == 0) return pkg;
            /* Handle versioned provides like 'libz.so.1=1.2.3' */
            if (strncmp(pname, name, strlen(name)) == 0 && pname[strlen(name)] == '=')
                return pkg;
            prov = prov->link;
        }
        curr = curr->link;
    }
    return NULL;
}

static pkg_info_t* find_installed_package(const char *name)
{
    llist_t *curr = G.installed_pkgs;
    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        if (strcmp(pkg->name, name) == 0) return pkg;
        curr = curr->link;
    }
    return NULL;
}

static int check_version_constraint(const char *inst_ver, const char *op, const char *req_ver)
{
    int cmp = compare_versions(inst_ver, req_ver);
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return cmp == 0;
    if (strcmp(op, ">=") == 0) return cmp >= 0;
    if (strcmp(op, "<=") == 0) return cmp <= 0;
    if (strcmp(op, ">") == 0) return cmp > 0;
    if (strcmp(op, "<") == 0) return cmp < 0;
    return 0;
}

static void resolve_package(const char *name, int force_install)
{
    pkg_info_t *pkg = find_package(name);
    pkg_info_t *installed;
    llist_t *dep;

    if (!pkg) {
        bb_error_msg("Package not found: %s", name);
        return;
    }
    if (pkg->state == 2) return;

    installed = find_installed_package(pkg->name);

    /* If it's already installed, we skip downloading it UNLESS:
     * 1. It was explicitly requested (force_install == 1)
     * 2. The user passed --reinstall
     * 3. We are doing a full system upgrade (-u) and a newer version exists */
    if (installed && !force_install && !(G.opts & OPT_reinstall)) {
        if (!(G.opts & OPT_u) || compare_versions(pkg->version, installed->version) <= 0) {
            return;
        }
    }

    pkg->state = 2; /* Mark as resolved to prevent infinite loops */

    dep = pkg->depends;
    while (dep) {
        char *dep_str = (char*)dep->data;
        char *dep_name = xstrdup(dep_str);
        char *op = strpbrk(dep_name, "<=>");
        char *req_ver = NULL;
        char op_str[3] = {0};

        /* Parse the exact operator and target version */
        if (op) {
            if (op[1] == '=' || op[1] == '>' || op[1] == '<') {
                op_str[0] = op[0]; op_str[1] = op[1]; req_ver = op + 2;
            } else {
                op_str[0] = op[0]; req_ver = op + 1;
            }
            *op = '\0'; /* Null-terminate the dependency name */
        }

        /* Check if the installed system already satisfies this constraint */
        pkg_info_t *inst = find_installed_package(dep_name);
        int satisfied = 0;

        if (inst) {
            if (!op) {
                satisfied = 1; /* Installed, and no specific version required */
            } else {
                satisfied = check_version_constraint(inst->version, op_str, req_ver);
            }
        }

        /* If the local system DOES NOT satisfy the constraint, force the upgrade */
        if (!satisfied) {
            resolve_package(dep_name, 1);
        }

        free(dep_name);
        dep = dep->link;
    }
    llist_add_to_end(&G.resolved_pkgs, pkg);
}

static void expand_groups(void)
{
    llist_t *new_targets = NULL;
    llist_t *curr = G.target_pkgs;

    while (curr) {
        char *target = (char*)curr->data;
        int found = 0;

        /* Check if it's a real package first */
        if (find_package(target)) {
            llist_add_to_end(&new_targets, xstrdup(target));
            found = 1;
        } else {
            /* Check if it's a group name */
            llist_t *pkgs = G.available_pkgs;
            while (pkgs) {
                pkg_info_t *pkg = (pkg_info_t*)pkgs->data;
                llist_t *g = pkg->groups;
                while (g) {
                    if (strcmp((char*)g->data, target) == 0) {
                        /* Check for duplicates in new_targets */
                        llist_t *t = new_targets;
                        int exists = 0;
                        while (t) {
                            if (strcmp((char*)t->data, pkg->name) == 0) {
                                exists = 1;
                                break;
                            }
                            t = t->link;
                        }
                        if (!exists)
                            llist_add_to_end(&new_targets, xstrdup(pkg->name));
                        found = 1;
                        break;
                    }
                    g = g->link;
                }
                pkgs = pkgs->link;
            }
        }

        if (!found) {
            /* Keep it so resolve_package can report error */
            llist_add_to_end(&new_targets, xstrdup(target));
        }
        curr = curr->link;
    }

    /* Free old list - simple way in busybox */
    while (G.target_pkgs) {
        free(llist_pop(&G.target_pkgs));
    }
    G.target_pkgs = new_targets;
}

static pacman_state_t do_resolve_deps(void)
{
    llist_t *curr;
    unsigned long long total_csize = 0;
    unsigned long long total_isize = 0;
    int count;

    load_local_db();
    expand_groups();

    if (G.opts & OPT_u) {
        printf(":: %sStarting full system upgrade...%s\n", CLR_BOLD, CLR_RESET);
        curr = G.installed_pkgs;
        while (curr) {
            pkg_info_t *inst = (pkg_info_t*)curr->data;
            pkg_info_t *available = find_package(inst->name);
            if (available && compare_versions(available->version, inst->version) > 0) {
                llist_add_to_end(&G.target_pkgs, xstrdup(inst->name));
            }
            curr = curr->link;
        }
    }

    curr = G.target_pkgs;
    if (!curr) {
        printf(" there is nothing to do\n");
        return PACMAN_STATE_CLEANUP;
    }

    printf(":: %sResolving dependencies...%s\n", CLR_BOLD, CLR_RESET);
    while (curr) {
        resolve_package((char*)curr->data, 1); /* Pass 1 to force resolution */
        curr = curr->link;
    }

    if (!G.resolved_pkgs) return PACMAN_STATE_FATAL;

    count = llist_count(G.resolved_pkgs);
    printf("\nPackages (%d)", count);
    curr = G.resolved_pkgs;
    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        printf("  %s-%s", pkg->name, pkg->version);
        total_csize += pkg->csize;
        total_isize += pkg->isize;
        curr = curr->link;
    }
    printf("\n\n");

    printf("Total Download Size:    %.2f MiB\n", (double)total_csize / (1024*1024));
    printf("Total Installed Size:   %.2f MiB\n", (double)total_isize / (1024*1024));

    printf("\n:: %sProceed with installation? [Y/n]%s ", CLR_BOLD, CLR_RESET);
    fflush(stdout);
    {
        int c = tolower(fgetc(stdin));
        if (c != 'y' && c != '\n' && c != '\r')
            return PACMAN_STATE_CLEANUP;
    }

    return PACMAN_STATE_DOWNLOAD;
}

static pacman_repo_t* find_repo(const char *name)
{
    llist_t *curr = G.repos;
    while (curr) {
        pacman_repo_t *repo = (pacman_repo_t*)curr->data;
        if (strcmp(repo->name, name) == 0) return repo;
        curr = curr->link;
    }
    return NULL;
}

static pacman_state_t do_download(void)
{
    llist_t *curr = G.resolved_pkgs;
    int total = llist_count(G.resolved_pkgs);
    int i = 1;

    printf(":: %sRetrieving packages...%s\n", CLR_BOLD, CLR_RESET);
    bb_make_directory(G.cachedir, 0755, FILEUTILS_RECUR);

    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        pacman_repo_t *repo = find_repo(pkg->repo_name);

        if (!pkg->filename) {
            bb_error_msg("corrupt database: no filename specified for %s", pkg->name);
            return PACMAN_STATE_FATAL;
        }

        if (repo && repo->servers) {
            char *url = substitute_vars((char*)repo->servers->data, repo->name);
            char *pkg_url = xasprintf("%s/%s", url, pkg->filename);
            char *dest = xasprintf("%s/%s", G.cachedir, pkg->filename);

            if (access(dest, F_OK) == 0) {
                printf(" Get:%d %s%s-%s%s (cached)\n", i++, CLR_GREEN, pkg->name, pkg->version, CLR_RESET);
            } else {
                printf(" Get:%d %s%s-%s%s downloading...\n", i++, CLR_GREEN, pkg->name, pkg->version, CLR_RESET);
                if (download_file(pkg_url, dest) != 0) {
                    bb_error_msg("failed to download %s", pkg->filename);
                    free(dest); free(pkg_url); free(url);
                    return PACMAN_STATE_FATAL;
                }
            }

            free(dest);
            free(pkg_url);
            free(url);
        }
        curr = curr->link;
    }
    return PACMAN_STATE_COMMIT;
}

static pacman_state_t do_commit(void)
{
    llist_t *curr = G.resolved_pkgs;
    int total = llist_count(G.resolved_pkgs);
    int i = 1;
    char *cmd;

    printf(":: %sProcessing package changes...%s\n", CLR_BOLD, CLR_RESET);

    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        char *pkg_file;
        char *local_db_dir;
        char *install_script;
        char *files_file;
        FILE *f_files;
        char *desc_file;
        FILE *f_desc;
        char *old_path, *new_path;
        char *work_tar;

        pkg_file = xasprintf("%s/%s", G.cachedir, pkg->filename);
        local_db_dir = xasprintf("%s/local/%s-%s", G.dbpath, pkg->name, pkg->version);

        /* 0. Cleanup old version metadata to prevent official pacman "duplicated database entry" errors */
        {
            pkg_info_t *old_inst = find_installed_package(pkg->name);
            if (old_inst && strcmp(old_inst->version, pkg->version) != 0) {
                char *old_db_dir = xasprintf("%s/local/%s-%s", G.dbpath, old_inst->name, old_inst->version);
                char *rm_c;
                rm_c = xasprintf("rm -rf \"%s\" 2>/dev/null || %s rm -rf \"%s\"", old_db_dir, G.bb_path, old_db_dir);
                system(rm_c);
                free(rm_c);
                free(old_db_dir);
            }
        }

        printf("(%d/%d) %sInstalling %s-%s...%s\n", i++, total, CLR_GREEN, pkg->name, pkg->version, CLR_RESET);

        if (strstr(pkg_file, ".zst")) {
            work_tar = xasprintf("%s.tar", pkg_file);
            if (unzstd_file(pkg_file, work_tar) != 0) {
                free(work_tar);
                free(local_db_dir);
                free(pkg_file);
                return PACMAN_STATE_FATAL;
            }
        } else {
            work_tar = xstrdup(pkg_file);
        }

        /* 1. Extract package files to root, excluding metadata that pollutes / */
        tar_file(G.root, work_tar, "--exclude='.*'", NULL);

        /* 2. Create local DB entry to prevent "blindness" and metadata pollution */
        bb_make_directory(local_db_dir, 0755, FILEUTILS_RECUR);

        /* Extract metadata specifically to the local DB directory.
         * .PKGINFO is guaranteed to exist. Others are optional and we try them silently. */
        tar_file(local_db_dir, work_tar, NULL, ".PKGINFO");
        {
            char *meta_cmd;
            meta_cmd = xasprintf("tar -C \"%s\" -xf \"%s\" .INSTALL .MTREE .CHANGELOG 2>/dev/null || "
                                       "%s tar -C \"%s\" -xf \"%s\" .INSTALL .MTREE .CHANGELOG 2>/dev/null || true",
                                       local_db_dir, work_tar, G.bb_path, local_db_dir, work_tar);
            system(meta_cmd);
            free(meta_cmd);
        }

        /* Create files list for removal with real pacman header */
        files_file = xasprintf("%s/files", local_db_dir);
        f_files = fopen(files_file, "w");
        if (f_files) {
            fprintf(f_files, "%%FILES%%\n");
            fclose(f_files);
            tar_list(work_tar, files_file, "--exclude='.*'", NULL);
        }
        free(files_file);

        /* Create desc file for real pacman compatibility */
        desc_file = xasprintf("%s/desc", local_db_dir);
        f_desc = fopen(desc_file, "w");
        if (f_desc) {
            llist_t *d, *p;
            fprintf(f_desc, "%%NAME%%\n%s\n\n", pkg->name);
            fprintf(f_desc, "%%VERSION%%\n%s\n\n", pkg->version);
            if (pkg->desc) fprintf(f_desc, "%%DESC%%\n%s\n\n", pkg->desc);
            if (pkg->url) fprintf(f_desc, "%%URL%%\n%s\n\n", pkg->url);
            fprintf(f_desc, "%%ARCH%%\n%s\n\n", G.arch);
            if (pkg->license) fprintf(f_desc, "%%LICENSE%%\n%s\n\n", pkg->license);
            fprintf(f_desc, "%%SIZE%%\n%lu\n\n", pkg->isize);

            if (pkg->depends) {
                fprintf(f_desc, "%%DEPENDS%%\n");
                d = pkg->depends;
                while (d) {
                    fprintf(f_desc, "%s\n", (char*)d->data);
                    d = d->link;
                }
                fprintf(f_desc, "\n");
            }
            if (pkg->provides) {
                fprintf(f_desc, "%%PROVIDES%%\n");
                p = pkg->provides;
                while (p) {
                    fprintf(f_desc, "%s\n", (char*)p->data);
                    p = p->link;
                }
                fprintf(f_desc, "\n");
            }
            fclose(f_desc);
        }
        free(desc_file);

        /* Rename metadata files for real pacman compatibility */
        old_path = xasprintf("%s/.MTREE", local_db_dir);
        new_path = xasprintf("%s/mtree", local_db_dir);
        if (access(old_path, F_OK) == 0) rename(old_path, new_path);
        free(old_path); free(new_path);

        /* 3. Execute .INSTALL script if it exists */
        install_script = xasprintf("%s/.INSTALL", local_db_dir);
        if (access(install_script, R_OK) == 0) {
            char *new_inst = xasprintf("%s/install", local_db_dir);
            char *script_path_in_chroot = new_inst + strlen(G.root);
            pkg_info_t *old_inst = find_installed_package(pkg->name);

            rename(install_script, new_inst);
            if (script_path_in_chroot[0] != '/') script_path_in_chroot--; /* Ensure leading slash */

            if (old_inst) {
                printf(" :: Running post-upgrade script for %s...\n", pkg->name);
                cmd = xasprintf("chroot %s sh -c \". %s; post_upgrade %s %s\"", G.root, script_path_in_chroot, pkg->version, old_inst->version);
            } else {
                printf(" :: Running post-install script for %s...\n", pkg->name);
                cmd = xasprintf("chroot %s sh -c \". %s; post_install %s\"", G.root, script_path_in_chroot, pkg->version);
            }
            system(cmd);
            free(cmd);
            free(new_inst);
        }
        free(install_script);

        if (strstr(pkg_file, ".zst")) unlink(work_tar);
        free(work_tar);
        free(local_db_dir);
        free(pkg_file);
        curr = curr->link;
    }

    /* 4. Update linker cache */
    printf(":: Updating linker cache...\n");
    {
        cmd = xasprintf("ldconfig 2>/dev/null || %s ldconfig", G.bb_path);
        system(cmd);
        free(cmd);
    }

    return PACMAN_STATE_CLEANUP;
}

static pacman_state_t do_info(void)
{
    llist_t *curr_target = G.target_pkgs;
    while (curr_target) {
        char *target = (char*)curr_target->data;
        pkg_info_t *pkg = find_package(target);
        if (!pkg) {
            load_local_db();
            pkg = find_installed_package(target);
        }

        if (pkg) {
            llist_t *p, *d;
            printf("Repository      : %s\n", pkg->repo_name);
            printf("Name            : %s\n", pkg->name);
            printf("Version         : %s\n", pkg->version);
            printf("Description     : %s\n", pkg->desc ? pkg->desc : "None");
            printf("Architecture    : %s\n", G.arch);
            printf("URL             : %s\n", pkg->url ? pkg->url : "None");
            printf("Licenses        : %s\n", pkg->license ? pkg->license : "None");
            printf("Groups          : ");
            {
                llist_t *g = pkg->groups;
                if (!g) printf("None");
                while (g) {
                    printf("%s ", (char*)g->data);
                    g = g->link;
                }
            }
            printf("\nProvides        : ");
            p = pkg->provides;
            while (p) {
                printf("%s ", (char*)p->data);
                p = p->link;
            }
            printf("\nDepends On      : ");
            d = pkg->depends;
            while (d) {
                printf("%s ", (char*)d->data);
                d = d->link;
            }
            printf("\nInstalled Size  : %.2f KiB\n", (double)pkg->isize / 1024);
            printf("MD5 Sum         : %s\n", pkg->md5sum ? pkg->md5sum : "None");
            printf("SHA-256 Sum     : %s\n", pkg->sha256sum ? pkg->sha256sum : "None");
            printf("\n");
        } else {
            bb_error_msg("package '%s' not found", target);
        }
        curr_target = curr_target->link;
    }
    return PACMAN_STATE_CLEANUP;
}

static pacman_state_t do_remove(void)
{
    llist_t *curr_target = G.target_pkgs;
    load_local_db();

    if (!curr_target) return PACMAN_STATE_CLEANUP;

    while (curr_target) {
        char *target = (char*)curr_target->data;
        pkg_info_t *pkg = find_installed_package(target);
        if (pkg) {
            char *local_db_dir = xasprintf("%s/local/%s-%s", G.dbpath, pkg->name, pkg->version);
            char *files_file = xasprintf("%s/files", local_db_dir);
            FILE *f = fopen(files_file, "r");
            char *rm_cmd;

            printf("removing %s-%s...\n", pkg->name, pkg->version);
            if (f) {
                char *line;
                while ((line = xmalloc_fgetline(f)) != NULL) {
                    char *path;
                    struct stat st;
                    if (line[0] == '%') {
                        free(line);
                        continue;
                    }
                    path = xasprintf("%s/%s", G.root, line);
                    if (lstat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
                        unlink(path);
                    }
                    free(path);
                    free(line);
                }
                fclose(f);
            } else {
                bb_error_msg("warning: file list for %s not found, only DB entry will be removed", pkg->name);
            }

            {
                rm_cmd = xasprintf("rm -rf %s 2>/dev/null || %s rm -rf %s", local_db_dir, G.bb_path, local_db_dir);
            }
            system(rm_cmd);
            free(rm_cmd);

            free(files_file);
            free(local_db_dir);
        } else {
            bb_error_msg("package not found: %s", target);
        }
        curr_target = curr_target->link;
    }
    return PACMAN_STATE_CLEANUP;
}

static pacman_state_t do_verify(void)
{
    llist_t *curr_target = G.target_pkgs;
    load_local_db();
    while (curr_target) {
        char *target = (char*)curr_target->data;
        pkg_info_t *pkg = find_installed_package(target);
        if (pkg) {
            char *local_db_dir, *files_file;
            FILE *f;
            int missing = 0;

            printf("Verifying %s-%s...\n", pkg->name, pkg->version);
            local_db_dir = xasprintf("%s/local/%s-%s", G.dbpath, pkg->name, pkg->version);
            files_file = xasprintf("%s/files", local_db_dir);
            f = fopen(files_file, "r");

            if (f) {
                char *line;
                while ((line = xmalloc_fgetline(f)) != NULL) {
                    char *path;
                    if (line[0] == '%') {
                        free(line);
                        continue;
                    }
                    path = xasprintf("%s/%s", G.root, line);
                    if (access(path, F_OK) != 0) {
                        printf("missing: %s\n", path);
                        missing++;
                    }
                    free(path);
                    free(line);
                }
                fclose(f);
                if (missing == 0) printf("all files present.\n");
                else printf("%d files missing.\n", missing);
            } else {
                printf("no file list found for %s\n", pkg->name);
            }

            if (G.opts & OPT_md5check) {
                char *md5_file = xasprintf("%s/md5sums", local_db_dir);
                if (access(md5_file, R_OK) == 0) {
                    char *cmd;
                    cmd = xasprintf("md5sum -c %s 2>/dev/null || %s md5sum -c %s", md5_file, G.bb_path, md5_file);
                    system(cmd);
                    free(cmd);
                } else {
                    printf("no md5sums found for %s\n", pkg->name);
                }
                free(md5_file);
            }

            free(files_file);
            free(local_db_dir);
        } else {
            bb_error_msg("package not installed: %s", target);
        }
        curr_target = curr_target->link;
    }
    return PACMAN_STATE_CLEANUP;
}

/* ==========================================================================
 * 5. THE CORE FSM ROUTER
 * ========================================================================== */

static void run_pacman_fsm(void)
{
    while (G.current_state != PACMAN_STATE_CLEANUP &&
           G.current_state != PACMAN_STATE_FATAL)
    {
        switch (G.current_state) {
            case PACMAN_STATE_INIT:
                if (getenv("PACMAN_CONF"))
                    load_pacman_conf(getenv("PACMAN_CONF"));
                else
                    load_pacman_conf("/etc/pacman.conf");

                if (G.opts & OPT_y)
                    G.current_state = PACMAN_STATE_SYNC_DB;
                else
                    G.current_state = PACMAN_STATE_PARSE_DB;
                break;
            case PACMAN_STATE_SYNC_DB:
                G.current_state = do_sync_db();
                break;
            case PACMAN_STATE_PARSE_DB:
                G.current_state = do_parse_db();
                if (G.current_state == PACMAN_STATE_RESOLVE_DEPS && (G.opts & OPT_i))
                    G.current_state = PACMAN_STATE_INFO;
                break;
            case PACMAN_STATE_SEARCH:
                G.current_state = do_search();
                break;
            case PACMAN_STATE_INFO:
                G.current_state = do_info();
                break;
            case PACMAN_STATE_RESOLVE_DEPS:
                G.current_state = do_resolve_deps();
                break;
            case PACMAN_STATE_DOWNLOAD:
                G.current_state = do_download();
                break;
            case PACMAN_STATE_COMMIT:
                G.current_state = do_commit();
                break;
            case PACMAN_STATE_REMOVE:
                G.current_state = do_remove();
                break;
            case PACMAN_STATE_VERIFY:
                G.current_state = do_verify();
                break;
            default:
                G.current_state = PACMAN_STATE_FATAL;
                break;
        }
    }
}

/* ==========================================================================
 * 6. BUSYBOX APPLET MAIN
 * ========================================================================== */

static const char pacman_longopts[] ALIGN1 =
    "reinstall\0"      No_argument "\xfe"
    "rescue-install\0"  No_argument "\xfd"
    "verify\0"          No_argument "\xfc"
    "md5check\0"        No_argument "\xfb"
    "\0"
    ;

int pacman_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int pacman_main(int argc, char **argv)
{
    setup_common_bufsiz();
    memset(&G, 0, sizeof(G));
    G.current_state = PACMAN_STATE_INIT;
    get_bb_exe(G.bb_path, sizeof(G.bb_path));

    /* Handle md5check as a subcommand if present */
    if (argc > 1 && strcmp(argv[1], "md5check") == 0) {
        G.opts |= OPT_md5check | OPT_verify;
        argv++;
    }

    G.opts |= getopt32long(argv, "SsyuRi\xfe\xfd\xfc\xfb", pacman_longopts);
    argv += optind;

    while (*argv) {
        llist_add_to_end(&G.target_pkgs, xstrdup(*argv));
        argv++;
    }

    if (G.opts & OPT_R)
        G.current_state = PACMAN_STATE_REMOVE;
    else if (G.opts & (OPT_verify | OPT_md5check))
        G.current_state = PACMAN_STATE_VERIFY;
    else if (!(G.opts & (OPT_S | OPT_i))) {
        bb_show_usage();
    }

    run_pacman_fsm();

    if (G.current_state == PACMAN_STATE_FATAL)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
