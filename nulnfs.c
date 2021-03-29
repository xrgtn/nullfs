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
#include <fuse/fuse_lowlevel_compat.h> // fuse_dirent_size in fuse-2.9.9
#include "linux_list.h" // list_head

time_t start_t;

struct nulnfs_inode {
    struct stat st;
    struct list_head r_ent;     /* list of referring dirents */
    struct list_head ls_ent;    /* list of child dirents */
    struct list_head free_ino;  /* free inodes */
};

struct nulnfs_dirent {
    struct dirent de;
    struct list_head ls_ent;    /* list of sibling dirents */
    fuse_ino_t p_ino;           /* parent inode */
    struct list_head free_ent;  /* free dirents */
};

struct nulnfs_inode *all_inodes = NULL;
LIST_HEAD(free_inodes);
struct nulnfs_dirent *all_dirents = NULL;
LIST_HEAD(free_dirents);

static struct fuse_lowlevel_ops nullfs_ll_ops;

static char *mountpoint = NULL;
int n_inodes = 65536;
int n_dirents = 65536;

/*
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
}

static void nullfs_mkdirstat(struct stat *pstat, fuse_req_t req,
fuse_ino_t i, mode_t m) {
    nullfs_mkstat(pstat, req, i, m);
    pstat->st_mode = (m & ~S_IFMT) | S_IFDIR;
    pstat->st_nlink = 2;
}
*/

/* remove dirent from dirnode's ls_ent list and return to
   filesystem's free_ent list. don't change st_nlink */
static int free_dirent(struct nulnfs_dirent *pdirent) {
    list_del_init(&pdirent->ls_ent);    /* remove from old dir */
    if (list_empty(&pdirent->free_ent))
        list_add_tail(&pdirent->free_ent, &free_dirents);
    return 0;   /* TODO: error reporting */
}

/* remove dirent from filesystem's free_ent list and append
   to dirnode's ls_ent list. don't change st_nlink */
static int insert_dirent_into_dirnode(struct nulnfs_dirent *pdirent,
struct nulnfs_inode *pinode) {
    if (! S_ISDIR(pinode->st.st_mode)) return ENOTDIR;
    list_del_init(&pdirent->ls_ent);    /* remove from old dir */
    list_del_init(&pdirent->free_ent);  /* remove from free dirents */
    list_add_tail(&pdirent->ls_ent, &pinode->ls_ent);
    return 0;   /* TODO: report ls_ent/free_ent collisions */
}

/* allocate dirent from filesystem's free_ent list */
static struct nulnfs_dirent *alloc_dirent(const char *name,
ino_t ino, ino_t p_ino, unsigned char d_type) {
    struct nulnfs_dirent *dirent;
    if (list_empty(&free_dirents)) {
        fprintf(stderr, "ERROR alloc_dirent \"%s\":"
            " no more free dirents\n", name);
        return NULL;
    };
    dirent = list_entry(free_dirents.next, struct nulnfs_dirent,
        free_ent);
    fprintf(stderr, "DEBUG alloc_dirent \"%s\":\ndirent=%p\n"
        "all_dirents=%p\n&free_dirents=%p\nfree_dirents.next=%p\n"
        "free_dirents.prev=%p\n&all_dirents->ls_ent=%p\n"
        "&all_dirents->free_ent=%p\n", name, dirent, all_dirents,
        &free_dirents, free_dirents.next, free_dirents.prev,
        &all_dirents->ls_ent, &all_dirents->free_ent);
    if (dirent->de.d_off != dirent - all_dirents + 1) {
        fprintf(stderr, "ERROR alloc_dirent \"%s\": #%i, off %li\n",
            name, (int)(dirent - all_dirents + 1), dirent->de.d_off);
        return NULL;
    };
    list_del_init(&dirent->free_ent);
    dirent->de.d_ino = ino;
    dirent->de.d_type = d_type;
    dirent->de.d_reclen = 0;     /* XXX */
    dirent->p_ino = p_ino;
    strncpy(dirent->de.d_name, name, sizeof(dirent->de.d_name));
    dirent->de.d_name[255] = '\0';
    return dirent;
}

static int init_dirnode(struct nulnfs_inode *pinode,
fuse_ino_t i, fuse_ino_t parent_ino, uid_t u, gid_t g, mode_t m) {
    struct nulnfs_dirent *p_d_ent, *p_dd_ent;
    pinode->st.st_ino = i;
    pinode->st.st_uid = u;
    pinode->st.st_gid = g;
    pinode->st.st_mode = (m & ~S_IFMT) | S_IFDIR;
    pinode->st.st_nlink = 2;
    pinode->st.st_mtime = time(NULL);
    pinode->st.st_ctime = pinode->st.st_mtime;
    pinode->st.st_atime = pinode->st.st_mtime;
    INIT_LIST_HEAD(&pinode->r_ent);
    INIT_LIST_HEAD(&pinode->ls_ent);
    list_del_init(&pinode->free_ino);
    p_d_ent = alloc_dirent(".", i, i, DT_DIR);
    if (p_d_ent == NULL) goto INIT_DIRNODE_ERR1;
    p_dd_ent = alloc_dirent("..", i, parent_ino ? parent_ino : i, DT_DIR);
    if (p_dd_ent == NULL) goto INIT_DIRNODE_ERR2;
    insert_dirent_into_dirnode(p_d_ent, pinode);
    insert_dirent_into_dirnode(p_dd_ent, pinode);
    return 1;
INIT_DIRNODE_ERR2:
    fprintf(stderr, "ERROR INIT_DIRNODE_ERR2\n");
    list_add(&p_dd_ent->free_ent, &free_dirents);
INIT_DIRNODE_ERR1:
    fprintf(stderr, "ERROR INIT_DIRNODE_ERR1\n");
    list_add(&pinode->free_ino, &free_inodes);
    return 0;
}

/* returns pointer to basename part of pathname */
const char *bnamepos(const char *name) {
    const char *p_slash;
    if (name[0] == '/' && name[1] == '\0') return name;
    p_slash = strrchr(name, '/');
    return (p_slash == NULL) ? name : p_slash + 1;
}

static void nullfs_ll_lookup(fuse_req_t req,
fuse_ino_t par_ino, const char *name) {
    fprintf(stderr, "DEBUG nullfs_ll_lookup: par_ino#%i, name %s\n",
        (int) par_ino, name);
    struct fuse_entry_param e;
    const struct nulnfs_dirent *de = NULL;
    const struct nulnfs_dirent *c;
    const char *bname = bnamepos(name);

    if (par_ino < 1 || par_ino > n_inodes) fuse_reply_err(req, ENOENT);

    __list_for_each_entry (c, &all_inodes[par_ino - 1].ls_ent, ls_ent) {
        if (0 == strcmp(bname, c->de.d_name)) {
            de = c;
            break;
        };
    };

    if (de != NULL) {
        e.ino = de->de.d_ino;
        e.attr_timeout = 1.0;
        e.entry_timeout = 1.0;
        memcpy(&e.attr, &all_inodes[de->de.d_ino].st,
            sizeof(struct stat));
        fuse_reply_entry(req, &e);
    };
}

/**
 * Open a directory
 *
 * Filesystem may store an arbitrary file handle (pointer, index,
 * etc) in fi->fh, and use this in other all other directory
 * stream operations (readdir, releasedir, fsyncdir).
 *
 * Filesystem may also implement stateless directory I/O and not
 * store anything in fi->fh, though that makes it impossible to
 * implement standard conforming directory stream operations in
 * case the contents of the directory can change between opendir
 * and releasedir.
 *
 * Valid replies:
 *   fuse_reply_open
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param fi file information
 */
static void nullfs_ll_opendir (fuse_req_t req, fuse_ino_t ino,
struct fuse_file_info *fi) {
    const struct nulnfs_inode *dinode;

    if (ino < 1 || ino > n_inodes) fuse_reply_err(req, ENOENT);
    if (fi == NULL) fuse_reply_err(req, EINVAL);
    dinode = all_inodes + ino - 1;
    if (! S_ISDIR(dinode->st.st_mode)) fuse_reply_err(req, ENOTDIR);
    fi->fh = (uint64_t) &dinode->ls_ent;
    fprintf(stderr, "DEBUG nullfs_ll_opendir ino#%lu: ls_ent=%p\n",
        ino, &dinode->ls_ent);
    fuse_reply_open(req, fi);
}

struct size_and_pos {
    size_t size;
    const struct list_head *pos;
};

static struct size_and_pos calculate_ls_buf(
const struct nulnfs_inode *dinode,
const struct list_head *pos, size_t max_size) {
    struct size_and_pos ls = {0, NULL};
    fprintf(stderr, "DEBUG calculate_ls_buf: dirno#%i, pos=%p, "
        "pos->next=%p\n", (int)(dinode->st.st_ino), pos, pos->next);
    for (ls.pos = pos->next; ls.pos != &dinode->ls_ent
    && ls.pos != pos; ls.pos = ls.pos->next) {
        fprintf(stderr, "DEBUG calculate_ls_buf: pos=%p, next=%p\n",
            ls.pos, ls.pos->next);
        const struct nulnfs_dirent *dirent = list_entry(ls.pos,
            const struct nulnfs_dirent, ls_ent);
        size_t e_size = fuse_dirent_size(strlen(dirent->de.d_name));
        if (ls.size + e_size > max_size) break;
        ls.size += e_size;
    };
    return ls;
}

/**
 * Read directory
 *
 * Send a buffer filled using fuse_add_direntry(), with size not
 * exceeding the requested size.  Send an empty buffer on end of
 * stream.
 *
 * fi->fh will contain the value set by the opendir method, or
 * will be undefined if the opendir method didn't set any value.
 *
 * Valid replies:
 *   fuse_reply_buf
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param size maximum number of bytes to send
 * @param off offset to continue reading the directory stream
 * @param fi file information
 */
static void nullfs_ll_readdir(fuse_req_t req, fuse_ino_t ino,
size_t size, off_t off, struct fuse_file_info *fi) {
    const struct nulnfs_inode *dinode;
    const struct list_head *ls_pos;
    struct size_and_pos ls_buf_end;
    char *ls_buf;

    if (ino < 1 || ino > n_inodes) fuse_reply_err(req, ENOENT);
    if (fi == NULL) fuse_reply_err(req, EINVAL);
    dinode = all_inodes + ino - 1;
    if (! S_ISDIR(dinode->st.st_mode)) fuse_reply_err(req, ENOTDIR);
    fprintf(stderr, "DEBUG readdir: ino#%i, offs %i, fh %p, sz %u\n",
        (int) ino, (int) off, (void *)fi->fh, (unsigned) size);
    if (off) {
        size_t filled_size = 0;
        if (off < 1 || off > n_dirents) fuse_reply_err(req, ENOENT);
        ls_pos = &all_dirents[off - 1].ls_ent;
    } else {
        ls_pos = &dinode->ls_ent;
    };
    fprintf(stderr, "DEBUG readdir: ls_pos=%p\n", ls_pos);
    ls_buf_end = calculate_ls_buf(dinode, ls_pos, size);
    if (ls_buf_end.size == 0) {
        fuse_reply_buf(req, NULL, 0);
    };
    ls_buf = malloc(ls_buf_end.size);
    if (ls_buf == NULL) {
        fuse_reply_err(req, ENOMEM);
    } else {
        char *buf_pos = ls_buf;
	for (ls_pos = (ls_pos)->next; ls_pos != ls_buf_end.pos;
        ls_pos = ls_pos->next) {
            const struct nulnfs_dirent *dirent = list_entry(ls_pos,
                const struct nulnfs_dirent, ls_ent);
            size_t entsize = fuse_add_direntry(req, buf_pos,
                ls_buf_end.size, dirent->de.d_name,
                &all_inodes[dirent->de.d_ino - 1].st,
                dirent->de.d_off);
            if (buf_pos - ls_buf + entsize > ls_buf_end.size) break;
            buf_pos += entsize;
        };
        fuse_reply_buf(req, ls_buf, buf_pos - ls_buf);
        free(ls_buf);
    };
}

/**
 * Get file attributes
 *
 * Valid replies:
 *   fuse_reply_attr
 *   fuse_reply_err
 *
 * @param req request handle
 * @param ino the inode number
 * @param fi for future use, currently always NULL
 */
static void nullfs_ll_getattr(fuse_req_t req, fuse_ino_t ino,
struct fuse_file_info *fi) {
    fprintf(stderr, "DEBUG nullfs_ll_getattr: ino#%i, fh %p\n",
        (int) ino, (void *)fi->fh);
    if (ino < 1 || ino > n_inodes) fuse_reply_err(req, ENOENT);

    fuse_reply_attr(req, &all_inodes[ino - 1].st, 1.0);
}

int init_fs(int n_inodes, int n_dirents) {
    int i;
    int e;

    start_t = time(NULL);

    memset(&nullfs_ll_ops, 0, sizeof(nullfs_ll_ops));
    nullfs_ll_ops.lookup = nullfs_ll_lookup;
    nullfs_ll_ops.getattr = nullfs_ll_getattr;
    nullfs_ll_ops.opendir = nullfs_ll_opendir;
    nullfs_ll_ops.readdir = nullfs_ll_readdir;

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
        INIT_LIST_HEAD(&(all_inodes[i].r_ent));
        INIT_LIST_HEAD(&(all_inodes[i].ls_ent));
        INIT_LIST_HEAD(&(all_inodes[i].free_ino));
        list_add_tail(&(all_inodes[i].free_ino), &free_inodes);
    };
    /* initialize all dirents and list them as free: */
    for (e = 0; e < n_dirents; e++) {
        all_dirents[e].de.d_off = e + 1;
        all_dirents[e].p_ino = 0;       /* no parent yet */
        INIT_LIST_HEAD(&(all_dirents[e].ls_ent));
        INIT_LIST_HEAD(&(all_dirents[e].free_ent));
        list_add_tail(&(all_dirents[e].free_ent), &free_dirents);
    };

    /* initialize root inode #1: */
    if (! init_dirnode(all_inodes + 0, 1, 0, 0, 0, 0755)) {
        fprintf(stderr, "ERROR: cannot initialize inode #1\n");
        free(all_inodes);
        free(all_dirents);
        return 3;
    };
    /* initialize root dirent */
    /*
    all_dirents[0].de.d_ino = 1;
    all_dirents[0].de.d_reclen = 1;
    all_dirents[0].de.d_type = DT_DIR;
    strcpy(all_dirents[0].de.d_name, "/");
    */

    return 0;
}

int main(int argc, char *argv[]) {
    int res;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_chan *ch;

    res = init_fs(n_inodes, n_dirents);
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
