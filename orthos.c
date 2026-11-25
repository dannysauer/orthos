/*******************************************************************************
 * $Id$
 *******************************************************************************
 * Orthos
 * Watch a Subversion export and log differences from SVN HEAD.
 *
 *******************************************************************************
 * Copyright 2014 Danny Sauer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#define _GNU_SOURCE
#include <poll.h>
#include "orthos.h"
#include <svn_client.h>
#include <svn_cmdline.h>
#include <svn_dirent_uri.h>
#include <svn_path.h>
#include <svn_hash.h>
#include <svn_pools.h>

#define WATCH_EVENTS (IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | \
                      IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB)

static struct mismatch* problems;
static svn_revnum_t revision = SVN_INVALID_REVNUM;
static char* snapshot;
static char* scratch;
static int handle;
static struct watch* watches;
static bool retry_scan;
static volatile sig_atomic_t stopping;

static void stop( int sig ){
    (void)sig;
    stopping = 1;
}

static void fail( const char* what ){
    perror(what);
    exit(EXIT_FAILURE);
}

static char* join( const char* dir, const char* name ){
    char* path = malloc(strlen(dir) + strlen(name) + 2);
    if( path == NULL ) fail("malloc");
    sprintf(path, "%s/%s", dir, name);
    return path;
}

/* Keep each alert on one line, even for odd filenames. */
static void report( const char* path, const char* reason ){
    struct mismatch* p;
    for( p = problems; p != NULL; p = p->next ){
        if( strcmp(p->path, path) == 0 ) break;
    }
    if( p == NULL ){
        p = calloc(1, sizeof(*p));
        if( p == NULL || (p->path = strdup(path)) == NULL ) fail("malloc");
        p->next = problems;
        problems = p;
    }
    p->seen = true;
    if( p->reason && !strcmp(p->reason, reason) ) return;
    printf("r%ld %s ", revision, reason);
    for( const unsigned char* c = (const unsigned char*)path; *c; c++ ){
        if( *c < 32 || *c == 127 || *c == '\\' ) printf("\\x%02x", *c);
        else putchar(*c);
    }
    putchar('\n');
    if( fflush(stdout) == EOF || ferror(stdout) ) fail("Writing alert to stdout");
    p->reason = reason;
}

static int remove_entry( const char* path, const struct stat* s, int type,
                         struct FTW* walk ){
    (void)s;
    (void)type;
    (void)walk;
    return remove(path);
}

static void cleanup(void){
    if( scratch != NULL ) nftw(scratch, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static bool svn_ok( svn_error_t* err ){
    if( err == NULL ) return true;
    svn_handle_error2(err, stderr, FALSE, "orthos: ");
    svn_error_clear(err);
    return false;
}

static svn_error_t* cancel( void* baton ){
    (void)baton;
    if( stopping ) return svn_error_create(SVN_ERR_CANCELLED, NULL, "Stopping");
    return SVN_NO_ERROR;
}

static svn_error_t* init_client( svn_client_ctx_t** ctx, apr_pool_t* pool ){
    apr_hash_t* config;
    SVN_ERR(svn_config_get_config(&config, NULL, pool));
    SVN_ERR(svn_client_create_context2(ctx, config, pool));
    (*ctx)->cancel_func = cancel;
    svn_config_t* cfg = svn_hash_gets(config, SVN_CONFIG_CATEGORY_CONFIG);
    return svn_cmdline_create_auth_baton2(&(*ctx)->auth_baton, TRUE, NULL, NULL,
            NULL, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, cfg, cancel, NULL, pool);
}

static svn_error_t* get_revision( void* baton, const char* path,
                                  const svn_client_info2_t* info, apr_pool_t* pool ){
    (void)path;
    (void)pool;
    *(svn_revnum_t*)baton = info->rev;
    return SVN_NO_ERROR;
}

/* Keep existing child alerts until we can check this part of the tree again. */
static void unverifiable( const char* path ){
    size_t len = strlen(path);
    for( struct mismatch* p = problems; p; p = p->next ){
        if( !strncmp(path, p->path, len) && p->path[len] == '/' ) p->seen = true;
    }
    report(path, "unverifiable");
    retry_scan = true;
}

static void compare_file( const char* fdpath, const char* repo, const char* local ){
    FILE* x = fopen(fdpath, "rb");
    if( x == NULL ){
        unverifiable(local);
        return;
    }
    FILE* y = fopen(repo, "rb");
    if( y == NULL ){
        if( errno != EMFILE && errno != ENFILE ) fail(repo);
        fclose(x);
        unverifiable(local);
        return;
    }
    char left[8192], right[8192];
    bool same;
    size_t n;
    do{
        n = fread(left, 1, sizeof(left), x);
        same = fread(right, 1, sizeof(right), y) == n && !memcmp(left, right, n);
    }while( same && n == sizeof(left) );
    if( ferror(y) ) fail(repo);
    if( ferror(x) ) unverifiable(local);
    else if( !same ) report(local, "contents differ");
    fclose(x);
    fclose(y);
}

static bool exists( const char* path, struct stat* s ){
    if( lstat(path, s) == 0 ) return true;
    if( errno != ENOENT && errno != ENOTDIR ) fail(path);
    return false;
}

static bool watch_dir( const char* fdpath ){
    int wd = inotify_add_watch(handle, fdpath, WATCH_EVENTS | IN_ONLYDIR);
    if( wd < 0 ) return false;
    struct watch* w = watches;
    while( w && w->wd != wd ) w = w->next;
    if( w == NULL ){
        w = malloc(sizeof(*w));
        if( w == NULL ) fail("malloc");
        w->next = watches;
        w->wd = wd;
        watches = w;
    }
    w->seen = true;
    return true;
}

static void retire_watches(void){
    struct watch** link = &watches;
    while( *link ){
        struct watch* w = *link;
        if( w->seen ){
            w->seen = false;
            link = &w->next;
        }else{
            if( inotify_rm_watch(handle, w->wd) < 0 && errno != EINVAL )
                fail("inotify_rm_watch");
            *link = w->next;
            free(w);
        }
    }
}

static void compare( int parent, const char* name, const char* local, const char* repo,
                     struct stat* identity );

/* The second pass picks up repository entries missing from the local directory. */
static bool children( DIR* dir, int parent, const char* local, const char* repo,
                      bool expected ){
    for(;;){
        errno = 0;
        struct dirent* entry = readdir(dir);
        if( !entry ) return errno == 0;
        const char* name = entry->d_name;
        if( !strcmp(name, ".") || !strcmp(name, "..") ) continue;
        struct stat s;
        if( expected && fstatat(parent, name, &s, AT_SYMLINK_NOFOLLOW) == 0 ) continue;
        char* a = join(local, name);
        char* b = repo ? join(repo, name) : NULL;
        compare(parent, name, a, b, NULL);
        free(a);
        free(b);
    }
}

static void compare( int parent, const char* name, const char* local, const char* repo,
                     struct stat* identity ){
    struct stat a, b;
    bool wanted = repo && exists(repo, &b);
    /* O_PATH pins even a symlink or FIFO without following it or opening it for I/O. */
    int fd = openat(parent, name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if( fd < 0 ){
        if( errno != ENOENT ) unverifiable(local);
        else if( wanted ) report(local, "missing");
        return;
    }
    if( fstat(fd, &a) < 0 ) fail("fstat");
    if( identity ) *identity = a;
    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", fd);
    const char* reason = NULL;
    if( !wanted ) reason = "unexpected";
    else if( (a.st_mode & S_IFMT) != (b.st_mode & S_IFMT) ) reason = "type differs";

    if( S_ISDIR(a.st_mode) ){
        bool watched = watch_dir(fdpath);
        DIR* dir = opendir(fdpath);
        const char* expected = reason ? NULL : repo;
        bool complete = dir && children(dir, dirfd(dir), local, expected, false);
        if( dir && expected ){
            DIR* reference = opendir(expected);
            if( reference ){
                if( !children(reference, dirfd(dir), local, expected, true) ) fail(expected);
                closedir(reference);
            }else{
                if( errno != EMFILE && errno != ENFILE ) fail(expected);
                complete = false;
            }
        }
        if( dir ) closedir(dir);
        if( !watched || !complete ){
            unverifiable(local);
            reason = NULL;
        }
    }else if( !reason && S_ISREG(a.st_mode) ){
        compare_file(fdpath, repo, local);
    }else if( !reason && S_ISLNK(a.st_mode) ){
        char x[PATH_MAX], y[PATH_MAX];
        ssize_t n = readlinkat(fd, "", x, sizeof(x));
        ssize_t m = readlink(repo, y, sizeof(y));
        if( m < 0 ) fail(repo);
        if( n < 0 ) unverifiable(local);
        else if( n != m || memcmp(x, y, n) ) reason = "link differs";
    }
    if( reason ) report(local, reason);
    close(fd);
}

/* Resolve the canonical root afresh in case the exporter replaced a parent. */
static void scan_root( const char* root, struct stat* identity ){
    char* path = strdup(root);
    if( !path ) fail("strdup");
    int parent = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if( parent < 0 ) fail("/");
    char* name = path + 1;
    char* slash;
    while( (slash = strchr(name, '/')) != NULL ){
        *slash = 0;
        int next = openat(parent, name, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(parent);
        parent = next;
        if( parent < 0 ) break;
        name = slash + 1;
    }
    if( parent < 0 ){
        unverifiable(root);
    }else{
        compare(parent, name, root, snapshot, identity);
        close(parent);
    }
    free(path);
}

int main( int argc, char* ARGV[] ){
    if( argc != 3 ){
        fprintf(stderr, "Usage: %s DIRECTORY SVN_URL\n", ARGV[0]);
        return 1;
    }
    const char* configured = ARGV[1];
    if( configured[0] != '/' ){
        char* cwd = getcwd(NULL, 0);
        if( !cwd ) fail("getcwd");
        configured = join(cwd, configured);
        free(cwd);
    }
    char* root = realpath(configured, NULL);
    if( root == NULL ) fail(ARGV[1]);
    static char temp[] = "/tmp/orthos-XXXXXX";
    if( !strcmp(root, "/") || !strcmp(root, "/tmp") ){
        fprintf(stderr, "The snapshot directory must be outside the watched tree\n");
        return 1;
    }
    free(root);
    scratch = mkdtemp(temp);
    if( scratch == NULL ) fail("mkdtemp");
    atexit(cleanup);
    snapshot = join(scratch, "export");
    handle = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if( handle < 0 ) fail("inotify_init1");
    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    if( svn_cmdline_init("orthos", stderr) != EXIT_SUCCESS ) return 1;
    apr_pool_t* pool = svn_pool_create(NULL);
    apr_pool_t* check = svn_pool_create(pool);
    svn_client_ctx_t* ctx;
    const char* url;
    if( !svn_path_is_url(ARGV[2]) ){
        fprintf(stderr, "Expected a Subversion repository URL\n");
        return 1;
    }
    if( !svn_ok(svn_uri_canonicalize_safe(&url, NULL, ARGV[2], pool, check)) ||
        !svn_ok(init_client(&ctx, pool)) ) return 1;
    signal(SIGPIPE, SIG_IGN);
    svn_opt_revision_t head = { .kind = svn_opt_revision_head };
    while( !stopping ){
        svn_pool_clear(check);
        svn_revnum_t latest = SVN_INVALID_REVNUM;
        if( !svn_ok(svn_client_info4(url, &head, &head, svn_depth_empty, FALSE,
                    FALSE, FALSE, NULL, get_revision, &latest, ctx, check)) ){
            if( stopping ) break;
            fprintf(stderr, "Could not verify SVN revision, retrying\n");
            sleep(5);
            continue;
        }
        if( latest != revision ){
            struct stat s;
            if( exists(snapshot, &s) && nftw(snapshot, remove_entry, 16, FTW_DEPTH | FTW_PHYS) )
                fail(snapshot);
            svn_opt_revision_t pinned = { .kind = svn_opt_revision_number,
                                          .value.number = latest };
            revision = SVN_INVALID_REVNUM;
            if( !svn_ok(svn_client_export5(NULL, url, snapshot, &pinned, &pinned,
                        FALSE, TRUE, FALSE, svn_depth_infinity, NULL, ctx, check)) ){
                if( stopping ) break;
                fprintf(stderr, "Could not verify SVN export, retrying\n");
                sleep(5);
                continue;
            }
            revision = latest;
        }
        retry_scan = false;
        struct stat identity = {0};
        root = realpath(configured, NULL);
        if( !root ){
            if( errno == ENOENT ) report(configured, "missing");
            else unverifiable(configured);
            retry_scan = true;
        }else{
            size_t len = strlen(scratch);
            if( !strcmp(root, "/") || !strcmp(root, "/tmp") ||
                (!strncmp(root, scratch, len) && (root[len] == 0 || root[len] == '/')) ){
                fprintf(stderr, "Export overlaps the temporary snapshot directory\n");
                return 1;
            }
            scan_root(root, &identity);
            free(root);
        }
        retire_watches();
        if( !watches ) retry_scan = true;
        struct mismatch** link = &problems;
        while( *link ){
            struct mismatch* p = *link;
            if( p->seen ){
                p->seen = false;
                link = &p->next;
            }else{
                *link = p->next;
                free(p->path);
                free(p);
            }
        }
        /* Every event, including queue overflow, triggers a complete comparison. */
        char events[8192];
        struct pollfd ready = { .fd = handle, .events = POLLIN };
        /* A signal can arrive just before poll. Bound the wait for shutdown. */
        int count;
        do{
            count = poll(&ready, 1, retry_scan ? 5000 : 1000);
            if( count == 0 && !retry_scan && !stopping ){
                struct stat current;
                if( stat(configured, &current) < 0 || current.st_dev != identity.st_dev ||
                    current.st_ino != identity.st_ino ) break;
            }
        }while( !count && !retry_scan && !stopping );
        if( count < 0 && errno != EINTR ) fail("inotify poll");
        if( count > 0 && read(handle, events, sizeof(events)) < 0 &&
            errno != EINTR && errno != EAGAIN ) fail("inotify read");
    }
    svn_pool_destroy(pool);
}
