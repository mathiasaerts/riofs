#include "include/fuse.h"
#include "include/dir_tree.h"

/*{{{ struct */
struct _S3Fuse {
    Application *app;
    DirTree *dir_tree;
    
    char *mountpoint;
    int multithreaded;
    int foreground;
    // the session that we use to process the fuse stuff
    struct fuse_session *session;
    struct fuse_chan *chan;
    // the event that we use to receive requests
    struct event *ev;
    // what our receive-message length is
    size_t recv_size;
    // the buffer that we use to receive events
    char *recv_buf;
};

static void s3fuse_on_read (evutil_socket_t fd, short what, void *arg);
static void s3fuse_readdir (fuse_req_t req, fuse_ino_t ino, 
    size_t size, off_t off, struct fuse_file_info *fi);
static void s3fuse_lookup (fuse_req_t req, fuse_ino_t parent, const char *name);
static void s3fuse_getattr (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
static void s3fuse_open (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi);
static void s3fuse_read (fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi);

static struct fuse_lowlevel_ops s3fuse_opers = {
	.readdir	= s3fuse_readdir,
	.lookup		= s3fuse_lookup,
    .getattr	= s3fuse_getattr,
	.open		= s3fuse_open,
	.read		= s3fuse_read,
};
/*}}}*/

/*{{{ init*/

// create S3Fuse object
// create fuse handle and add it to libevent polling
S3Fuse *s3fuse_create (Application *app, int argc, char *argv[])
{
    S3Fuse *s3fuse;
    struct fuse_args fuse_args = FUSE_ARGS_INIT(argc, argv);
    char *mountpoint;
    int multithreaded;
    int foreground;

    s3fuse = g_new0 (S3Fuse, 1);
    s3fuse->app = app;
    s3fuse->dir_tree = application_get_dir_tree (app);
    
    if (fuse_parse_cmdline (&fuse_args, &mountpoint, &multithreaded, &foreground) == -1) {
        LOG_err ("fuse_parse_cmdline");
        return NULL;
    }

    if ((s3fuse->chan = fuse_mount (mountpoint, &fuse_args)) == NULL) {
        LOG_err ("fuse_mount_common");
        return NULL;
    }

    // the receive buffer stufff
    s3fuse->recv_size = fuse_chan_bufsize (s3fuse->chan);

    // allocate the recv buffer
    if ((s3fuse->recv_buf = g_malloc (s3fuse->recv_size)) == NULL) {
        LOG_err ("failed to malloc memory !");
        return NULL;
    }
    
    // allocate a low-level session
    if ((s3fuse->session = fuse_lowlevel_new (&fuse_args, &s3fuse_opers, sizeof (s3fuse_opers), s3fuse)) == NULL) {
        LOG_err ("fuse_lowlevel_new");
        return NULL;
    }
    
    fuse_session_add_chan (s3fuse->session, s3fuse->chan);

    if ((s3fuse->ev = event_new (application_get_evbase (app), fuse_chan_fd (s3fuse->chan), EV_READ, &s3fuse_on_read, s3fuse)) == NULL) {
        LOG_err ("event_new");
        return NULL;
    }

    if (event_add (s3fuse->ev, NULL)) {
        LOG_err ("event_add");
        return NULL;
    }

    return s3fuse;
}

// low level fuse reading operations
static void s3fuse_on_read (evutil_socket_t fd, short what, void *arg)
{
    S3Fuse *s3fuse = (S3Fuse *)arg;
    struct fuse_chan *ch = s3fuse->chan;
    int res;

    if (!ch) {
        LOG_err ("OPS");
        return;
    }
    
    // loop until we complete a recv
    do {
        // a new fuse_req is available
        res = fuse_chan_recv (&ch, s3fuse->recv_buf, s3fuse->recv_size);
    } while (res == -EINTR);

    if (res == 0)
        LOG_err("fuse_chan_recv gave EOF");

    if (res < 0 && res != -EAGAIN)
        LOG_err("fuse_chan_recv failed: %s", strerror(-res));
    
    if (res > 0) {
        LOG_msg("got %d bytes from /dev/fuse", res);

        // received a fuse_req, so process it
        fuse_session_process (s3fuse->session, s3fuse->recv_buf, res, ch);
    }
    
    // reschedule
    if (event_add (s3fuse->ev, NULL))
        LOG_err("event_add");

    // ok, wait for the next event
    return;
}
/*}}}*/

/*{{{ readdir operation */

#define min(x, y) ((x) < (y) ? (x) : (y))

// return newly allocated buffer which holds directory entry
void s3fuse_add_dirbuf (fuse_req_t req, struct dirbuf *b, const char *name, fuse_ino_t ino)
{
    struct stat stbuf;
    size_t oldsize = b->size;
    
    LOG_debug ("add_dirbuf  ino: %d, name: %s", ino, name);

    // get required buff size
	b->size += fuse_add_direntry (req, NULL, 0, name, NULL, 0);

    // extend buffer
	b->p = (char *) g_realloc (b->p, b->size);
	memset (&stbuf, 0, sizeof (stbuf));
	stbuf.st_ino = ino;
    // add entry
	fuse_add_direntry (req, b->p + oldsize, b->size - oldsize, name, &stbuf, b->size);
}

// readdir callback
// Valid replies: fuse_reply_buf() fuse_reply_err()
static void s3fuse_readdir_cb (fuse_req_t req, gboolean success, size_t max_size, off_t off, const char *buf, size_t buf_size)
{
    LOG_debug ("readdir_cb  success: %s, buf_size: %zd, size: %zd, off: %d", success?"YES":"NO", buf_size, max_size, off);

    if (!success) {
		fuse_reply_err (req, ENOTDIR);
        return;
    }

	if (off < buf_size)
		fuse_reply_buf (req, buf + off, min (buf_size - off, max_size));
	else
	    fuse_reply_buf (req, NULL, 0);
}

// FUSE lowlevel operation: readdir
// Valid replies: fuse_reply_buf() fuse_reply_err()
static void s3fuse_readdir (fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
    S3Fuse *s3fuse = fuse_req_userdata (req);

    LOG_debug ("readdir  inode: %d, size: %zd, off: %d", ino, size, off);
    
    // fill directory buffer for "ino" directory
    dir_tree_fill_dir_buf (s3fuse->dir_tree, ino, size, off, s3fuse_readdir_cb, req);
}
/*}}}*/

/*{{{ getattr operation */

// getattr callback
static void s3fuse_getattr_cb (fuse_req_t req, gboolean success, fuse_ino_t ino, int mode, off_t file_size)
{
    struct stat stbuf;

    LOG_debug ("getattr_cb  success: %s", success?"YES":"NO");
    if (!success) {
		fuse_reply_err (req, ENOENT);
        return;
    }
    memset (&stbuf, 0, sizeof(stbuf));
    stbuf.st_ino = ino;
    stbuf.st_mode = mode;
	stbuf.st_nlink = 1;
	stbuf.st_size = file_size;
    
    fuse_reply_attr (req, &stbuf, 1.0);
}

// FUSE lowlevel operation: getattr
// Valid replies: fuse_reply_attr() fuse_reply_err()
static void s3fuse_getattr (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    S3Fuse *s3fuse = fuse_req_userdata (req);

    dir_tree_getattr (s3fuse->dir_tree, ino, s3fuse_getattr_cb, req);
}
/*}}}*/

/*{{{ lookup operation*/

// lookup callback
static void s3fuse_lookup_cb (fuse_req_t req, gboolean success, fuse_ino_t ino, int mode, off_t file_size)
{
	struct fuse_entry_param e;

    LOG_debug ("lookup_cb  success: %s", success?"YES":"NO");
    if (!success) {
		fuse_reply_err (req, ENOENT);
        return;
    }

    memset(&e, 0, sizeof(e));
    e.ino = ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    e.attr.st_ino = ino;
    e.attr.st_mode = mode;
	e.attr.st_nlink = 1;
	e.attr.st_size = file_size;

    fuse_reply_entry (req, &e);
}

// FUSE lowlevel operation: lookup
// Valid replies: fuse_reply_entry() fuse_reply_err()
static void s3fuse_lookup (fuse_req_t req, fuse_ino_t parent, const char *name)
{
    S3Fuse *s3fuse = fuse_req_userdata (req);

    LOG_debug ("lookup  name: %s parent inode: %d", name, parent);

    dir_tree_lookup (s3fuse->dir_tree, parent, name, s3fuse_lookup_cb, req);
}
/*}}}*/

/*{{{ open operation */

// FUSE lowlevel operation: open
// Valid replies: fuse_reply_open() fuse_reply_err()
static void s3fuse_open (fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    LOG_debug ("open  inode: %d, flags: %d", ino, fi->flags);

    fuse_reply_open (req, fi);
}
/*}}}*/

/*{{{ read operation */

// read callback
static void s3fuse_read_cb (fuse_req_t req, gboolean success, size_t max_size, off_t off, const char *buf, size_t buf_size)
{

    LOG_debug ("read_cb  success: %s", success?"YES":"NO");
    if (!success) {
		fuse_reply_err (req, ENOENT);
        return;
    }

	if (off < buf_size)
		fuse_reply_buf (req, buf + off, min (buf_size - off, max_size));
	else
	    fuse_reply_buf (req, NULL, 0);
}

// FUSE lowlevel operation: read
// Valid replies: fuse_reply_buf() fuse_reply_err()
static void s3fuse_read (fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi)
{
    S3Fuse *s3fuse = fuse_req_userdata (req);
    
    LOG_debug ("read  inode: %d, size: %zd, off: %ld ", ino, size, off);

    dir_tree_read (s3fuse->dir_tree, ino, size, off, s3fuse_read_cb, req);
}
/*}}}*/
