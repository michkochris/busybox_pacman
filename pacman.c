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
//config:	pacman is a package manager for Arch Linux and its derivatives.
//config:	This implementation is a lightweight frontend for BusyBox.

//applet:IF_PACMAN(APPLET(pacman, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_PACMAN) += pacman.o

//usage:#define pacman_trivial_usage
//usage:       "[-Ssyu] [PACKAGE...]"
//usage:#define pacman_full_usage "\n\n"
//usage:       "Arch Linux package manager\n"
//usage:     "\nOptions:"
//usage:     "\n	-S	Synchronize packages"
//usage:     "\n	-s	Search for packages"
//usage:     "\n	-y	Refresh package databases"
//usage:     "\n	-u	Upgrade installed packages"

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
    PACMAN_STATE_RESOLVE_DEPS, /* Build the dependency graph */
    PACMAN_STATE_DOWNLOAD,     /* Fetch .pkg.tar.zst files */
    PACMAN_STATE_COMMIT,       /* Extract and run ALPM hooks (pre/post install) */
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
    unsigned long csize;
    unsigned long isize;
    char *repo_name;
    int state; /* 0: available, 1: target, 2: resolved */
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
} FIX_ALIASING;
#define G (*(struct globals*)bb_common_bufsiz1)

#define OPT_S (1 << 0)
#define OPT_s (1 << 1)
#define OPT_y (1 << 2)
#define OPT_u (1 << 3)

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

    if (!G.dbpath) G.dbpath = xstrdup("/var/lib/pacman/");
    if (!G.cachedir) G.cachedir = xstrdup("/var/cache/pacman/pkg/");
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
            } else if (strcmp(current_key, "CSIZE") == 0) {
                pkg->csize = atol(line);
            } else if (strcmp(current_key, "ISIZE") == 0) {
                pkg->isize = atol(line);
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
    char *local_path = xasprintf("%slocal", G.dbpath);
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
    const char *downloader = getenv("BB_DOWNLOADER");

    if (downloader && strcmp(downloader, "busybox") == 0) {
        cmd = xasprintf("busybox wget -q -O %s \"%s\"", dest, url);
        rc = system(cmd);
        free(cmd);
        return rc;
    }

    if (downloader && strcmp(downloader, "wget") == 0) {
        cmd = xasprintf("wget -q -O %s \"%s\"", dest, url);
        rc = system(cmd);
        free(cmd);
        return rc;
    }

    /* Default: Try system wget (silence stderr to avoid noisy library errors)
     * Fallback to internal busybox wget if system wget fails.
     */
    cmd = xasprintf("wget -q -O %s \"%s\" 2>/dev/null || %s wget -q -O %s \"%s\"",
                    dest, url, bb_busybox_exec_path, dest, url);
    rc = system(cmd);
    free(cmd);
    return rc;
}

static pacman_state_t do_sync_db(void) 
{
    llist_t *curr_repo = G.repos;
    char *sync_path = xasprintf("%ssync", G.dbpath);

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
    DIR *dir;
    struct dirent *entry;
    char *sync_path;

    sync_path = xasprintf("%ssync", G.dbpath);

    dir = opendir(sync_path);
    if (!dir) {
        free(sync_path);
        return PACMAN_STATE_FATAL;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".db")) {
            char *repo_name = xstrdup(entry->d_name);
            char *dot = strchr(repo_name, '.');
            char *db_file;
            archive_handle_t *handle;

            if (dot) *dot = '\0';

            G.current_repo_name = repo_name;
            db_file = xasprintf("%s/%s", sync_path, entry->d_name);

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
            free(db_file);
            free(repo_name);
        }
    }
    closedir(dir);
    free(sync_path);

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
    llist_t *curr = G.available_pkgs;
    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        llist_t *prov;

        if (strcmp(pkg->name, name) == 0) return pkg;

        /* Check if any package provides this name (capabilities/virtual packages) */
        prov = pkg->provides;
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

static void resolve_package(const char *name)
{
    pkg_info_t *pkg = find_package(name);
    pkg_info_t *installed;
    llist_t *dep;

    if (!pkg) {
        bb_error_msg("Package not found: %s", name);
        return;
    }
    if (pkg->state == 2) return;

    /* Check if already installed and up to date */
    installed = find_installed_package(pkg->name);
    if (installed && compare_versions(pkg->version, installed->version) <= 0) {
        /* Check if it's explicitly in target_pkgs (user wants to force/reinstall) */
        llist_t *t = G.target_pkgs;
        int forced = 0;
        while (t) {
            if (strcmp((char*)t->data, pkg->name) == 0) {
                forced = 1;
                break;
            }
            t = t->link;
        }
        if (!forced) return;
    }

    pkg->state = 2;
    dep = pkg->depends;
    while (dep) {
        char *dep_name = xstrdup((char*)dep->data);
        char *p = strpbrk(dep_name, "<=>");
        if (p) *p = '\0';

        resolve_package(dep_name);
        free(dep_name);
        dep = dep->link;
    }
    llist_add_to_end(&G.resolved_pkgs, pkg);
}

static pacman_state_t do_resolve_deps(void) 
{
    llist_t *curr;
    unsigned long long total_csize = 0;
    unsigned long long total_isize = 0;
    int count;

    load_local_db();

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
        resolve_package((char*)curr->data);
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
    printf(":: %sRetrieving packages...%s\n", CLR_BOLD, CLR_RESET);
    bb_make_directory(G.cachedir, 0755, FILEUTILS_RECUR);

    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        pacman_repo_t *repo = find_repo(pkg->repo_name);
        if (repo && repo->servers) {
            char *url = substitute_vars((char*)repo->servers->data, repo->name);
            char *pkg_url = xasprintf("%s/%s", url, pkg->filename ? pkg->filename : "");
            char *dest = xasprintf("%s/%s", G.cachedir, pkg->filename ? pkg->filename : "");

            printf(" %s%s-%s%s downloading...\n", CLR_GREEN, pkg->name, pkg->version, CLR_RESET);
            download_file(pkg_url, dest);

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

    printf(":: %sProcessing package changes...%s\n", CLR_BOLD, CLR_RESET);

    while (curr) {
        pkg_info_t *pkg = (pkg_info_t*)curr->data;
        char *pkg_file;
        char *local_db_dir;
        char *cmd;
        char *install_script;

        pkg_file = xasprintf("%s/%s", G.cachedir, pkg->filename);
        local_db_dir = xasprintf("%slocal/%s-%s", G.dbpath, pkg->name, pkg->version);

        printf("(%d/%d) %sInstalling %s-%s...%s\n", i++, total, CLR_GREEN, pkg->name, pkg->version, CLR_RESET);

        /* 1. Extract package files to root, excluding metadata that pollutes / */
        cmd = xasprintf("tar -C %s --exclude='.*' -xaf %s 2>/dev/null || "
                        "busybox tar -C %s --exclude='.*' -xaf %s",
                        G.root, pkg_file, G.root, pkg_file);
        system(cmd);
        free(cmd);

        /* 2. Create local DB entry to prevent "blindness" and metadata pollution (To-Do 1 & 2) */
        bb_make_directory(local_db_dir, 0755, FILEUTILS_RECUR);

        /* Extract metadata specifically to the local DB directory */
        cmd = xasprintf("tar -C %s -xaf %s .PKGINFO .INSTALL .MTREE .BUILDINFO 2>/dev/null || "
                        "busybox tar -C %s -xaf %s .PKGINFO .INSTALL .MTREE .BUILDINFO 2>/dev/null",
                        local_db_dir, pkg_file, local_db_dir, pkg_file);
        system(cmd);
        free(cmd);

        /* 3. Execute .INSTALL script if it exists (To-Do 3) */
        install_script = xasprintf("%s/.INSTALL", local_db_dir);
        if (access(install_script, X_OK) == 0) {
            cmd = xasprintf("chroot %s %s post_install %s", G.root, install_script + strlen(G.root), pkg->version);
            system(cmd);
            free(cmd);
        }
        free(install_script);

        free(local_db_dir);
        free(pkg_file);
        curr = curr->link;
    }

    /* 4. Update linker cache (To-Do 3 - Critical) */
    printf(":: Updating linker cache...\n");
    system("ldconfig 2>/dev/null || busybox ldconfig");

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
                break;
            case PACMAN_STATE_SEARCH:
                G.current_state = do_search();
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
            default:
                G.current_state = PACMAN_STATE_FATAL;
                break;
        }
    }
}

/* ==========================================================================
 * 6. BUSYBOX APPLET MAIN
 * ========================================================================== */

int pacman_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int pacman_main(int argc UNUSED_PARAM, char **argv)
{
    setup_common_bufsiz();
    memset(&G, 0, sizeof(G));
    G.current_state = PACMAN_STATE_INIT;

    G.opts = getopt32(argv, "Ssyu");
    argv += optind;

    while (*argv) {
        llist_add_to_end(&G.target_pkgs, xstrdup(*argv));
        argv++;
    }

    if (!(G.opts & OPT_S)) {
        bb_show_usage();
    }

    run_pacman_fsm();

    if (G.current_state == PACMAN_STATE_FATAL)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
