/*
    FUSE: Filesystem in Userspace
    Copyright (C) 2001-2005  Miklos Szeredi <miklos@szeredi.hu>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#define _FILE_OFFSET_BITS 64
#define FUSE_USE_VERSION 26
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <fuse/fuse_lowlevel.h>
#include "linux_list.h"

time_t start_t;

struct nulnfs_inode {
    struct stat st;
    struct list_head pe;        /* list of parent dirents */
    struct list_head ce;        /* list of child dirents */
    struct list_head fi;        /* free inodes */
};

struct nulnfs_dirent {
    struct dirent de;
    struct list_head se;        /* list of sibling dirents */
    fuse_ino_t p_ino;           /* parent inode */
    struct list_head fe;        /* free dirents */
};

struct nulnfs_inode *all_inodes = NULL;
LIST_HEAD(free_inodes);
struct nulnfs_dirent *all_dirents = NULL;
LIST_HEAD(free_dirents);

static struct fuse_lowlevel_ops nullfs_ll_ops;

static char *mountpoint = NULL;
int n_inodes = 65536;
int n_dirents = 65536;

static void nullfs_mkstat(struct stat *pstat, fuse_req_t req,
fuse_ino_t i, mode_t m) {
    const struct fuse_ctx *c = fuse_req_ctx(req);
    pstat->st_ino = i;
    if (c != NULL) {
        pstat->st_uid = c->uid;
        pstat->st_gid = c->gid;
    } else {
        pstat->st_uid = 0;
        pstat->st_gid = 0;
    }
    pstat->st_mode = m;
    pstat->st_nlink = 1;
    pstat->st_mtime = time(NULL);
    pstat->st_ctime = pstat->st_mtime;
    pstat->st_atime = pstat->st_mtime;
};

static void nullfs_mkdirstat(struct stat *pstat, fuse_req_t req,
fuse_ino_t i, mode_t m) {
    nullfs_mkstat(pstat, req, i, m);
    pstat->st_mode = (m & ~S_IFMT) | S_IFDIR;
    pstat->st_nlink = 2;
};

static void nullfs_mkdirnode(struct nulnfs_inode *pinode,
fuse_ino_t i, uid_t u, gid_t g, mode_t m) {
    pinode->st.st_uid = u;
    pinode->st.st_gid = g;
    pinode->st.st_mode = (m & ~S_IFMT) | S_IFDIR;
    pinode->st.st_nlink = 2;
    pinode->st.st_mtime = time(NULL);
    pinode->st.st_ctime = pinode->st.st_mtime;
    pinode->st.st_atime = pinode->st.st_mtime;
    INIT_LIST_HEAD(&(pinode->pe));
    INIT_LIST_HEAD(&(pinode->ce));
    list_del_init(&(pinode->fi));
};

const char *bnamepos(const char *name) {
    const char *p_slash;
    if (name[0] == '/' && name[1] == '\0') return name;
    p_slash = strrchr(name, '/');
    return (p_slash == NULL) ? name : p_slash + 1;
}

static void nullfs_ll_lookup(fuse_req_t req,
fuse_ino_t par_ino, const char *name) {
    struct fuse_entry_param e;
    const struct nulnfs_dirent *de = NULL;
    const struct nulnfs_dirent *c;
    const char *bname = bnamepos(name);

    if (par_ino < 1 || par_ino > n_inodes) fuse_reply_err(req, ENOENT);

    __list_for_each_entry (c, &(all_inodes[par_ino - 1].ce), se) {
        if (0 == strcmp(bname, c->de.d_name)) {
            de = c;
            break;
        };
    };

    if (de != NULL) {
        e.ino = de->de.d_ino;
        e.attr_timeout = 1.0;
        e.entry_timeout = 1.0;
        memcpy(&(e.attr), &(all_inodes[de->de.d_ino].st),
            sizeof(struct stat));
        fuse_reply_entry(req, &e);
    };
}

int nullfs_init(int n_inodes, int n_dirents) {
    int i;
    int e;

    start_t = time(NULL);

    memset(&nullfs_ll_ops, 0, sizeof(nullfs_ll_ops));
    nullfs_ll_ops.lookup = nullfs_ll_lookup;

    all_inodes = (struct nulnfs_inode *)
        malloc(sizeof(struct nulnfs_inode) * n_inodes);
    if (all_inodes == NULL) {
        fprintf(stderr, "ERROR: cannot allocate %i inodes\n", n_inodes);
        return 1;
    };
    memset(all_inodes, 0, sizeof(struct nulnfs_inode) * n_inodes);

    all_dirents = (struct nulnfs_dirent *)
        malloc(sizeof(struct nulnfs_dirent) * n_dirents);
    if (all_dirents == NULL) {
        fprintf(stderr, "ERROR: cannot allocate %i dirents\n", n_dirents);
        free(all_inodes);
        return 2;
    };

    /* initialize all inodes anf list them as free: */
    for (i = 0; i < n_inodes; i++) {
        all_inodes[i].st.st_ino = i + 1;
        INIT_LIST_HEAD(&(all_inodes[i].pe));
        INIT_LIST_HEAD(&(all_inodes[i].ce));
        INIT_LIST_HEAD(&(all_inodes[i].fi));
        list_add_tail(&(all_inodes[i].fi), &free_inodes);
    };
    /* initialize inode #1 and root inode (#2): */
    nullfs_mkdirnode(all_inodes + 0, 1, 0, 0, 0000);
    nullfs_mkdirnode(all_inodes + 1, 2, 0, 0, 0755);

    /* initialize all dirents and list them as free: */
    for (e = 0; e < n_dirents; e++) {
        all_dirents[e].de.d_off = e + 1;
        all_dirents[e].p_ino = 0;       /* no parent yet */
        INIT_LIST_HEAD(&(all_dirents[e].se));
        INIT_LIST_HEAD(&(all_dirents[e].fe));
        list_add_tail(&(all_dirents[e].fe), &free_dirents);
    };
    /* initialize root dirent */
    all_dirents[0].de.d_ino = 1;
    all_dirents[0].de.d_reclen = 1;
    all_dirents[0].de.d_type = DT_DIR;
    strcpy(all_dirents[0].de.d_name, "/");

    return 0;
}

int main(int argc, char *argv[]) {
    int res;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_chan *ch;

    res = nullfs_init(n_inodes, n_dirents);
    if (res) return res;

    /* TODO: implement cmdline options for setting custom
       n_inodes and n_dirents values */
    if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1
    && (ch = fuse_mount(mountpoint, &args)) != NULL) {
        struct fuse_session *se;

        se = fuse_lowlevel_new(&args, &nullfs_ll_ops,
            sizeof(nullfs_ll_ops), NULL);

        if (se != NULL) {
            if (fuse_set_signal_handlers(se) != -1) {
                fuse_session_add_chan(se, ch);
                res = fuse_session_loop(se);
                fuse_remove_signal_handlers(se);
                fuse_session_remove_chan(ch);
            }
            fuse_session_destroy(se);
        } else res = 3;

        fuse_unmount(mountpoint, ch);
    }
    fuse_opt_free_args(&args);

    return res;
}

/* vi:set sw=4 et tw=72: */
