#include "jitcInt.h"
#define FUSE_USE_VERSION 34
#include <fuse.h>
//#include <fuse_lowlevel.h>
#include <sys/mount.h>

TCL_DECLARE_MUTEX(g_memfs_init_mutex);
TCL_DECLARE_MUTEX(g_memfs_startup_mutex);
Tcl_Condition	startup = NULL;

char			template[] = P_tmpdir "/memfs_XXXXXX";
char*			mountpoint = NULL;

int				g_initialized = 0;
int				g_thread_ready = 0;
int				g_thread_startup_result = -1;
Tcl_ThreadId	g_memfs_tid = NULL;
struct fuse*	fuse = NULL;


static void* memfs_init(struct fuse_conn_info* conn, struct fuse_config* cfg) //{{{
{
	//fprintf(stderr, "memfs_init\n");
	return NULL;
}

//}}}
static int memfs_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) //{{{
{
	int			res = 0;

	//fprintf(stderr, "memfs_getattr: (%s)\n", path);
	if (strcmp(path, "/") == 0) {
		*stbuf = (struct stat){
			.st_mode	= S_IFDIR | 0755,
			.st_nlink	= 2
		};
	} else if (strcmp(path, "/foo") == 0) {
		*stbuf = (struct stat){
			.st_mode	= S_IFREG | 0644,
			.st_nlink	= 1,
			.st_size	= strlen("hello, world")
		};
	} else {
		res = -ENOENT;
	}

	return res;
}

//}}}
static int memfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) //{{{
{
	//fprintf(stderr, "memfs_readdir: (%s)\n", path);
	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	filler(buf, "foo", NULL, 0, 0);

	return 0;
}

//}}}
static int memfs_open(const char* path, struct fuse_file_info* fi) //{{{
{
	//fprintf(stderr, "memfs_open: (%s)\n", path);
	if (strcmp(path, "/foo") != 0)
		return -ENOENT;

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;

	return 0;
}

//}}}
static int memfs_read(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) //{{{
{
	const char	contents[] = "hello, world";

	//fprintf(stderr, "memfs_read: (%s)\n", path);
	if (strcmp(path, "/foo") != 0)
		return -ENOENT;

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
		return -EACCES;

	size_t len = strlen(contents);
	if (offset < len) {
		if (offset + size > len) size = len - offset;
		memcpy(buf, contents + offset, size);
	} else {
		size = 0;
	}

	return size;
}

//}}}

static const struct fuse_operations memfs_ops = {
	.init		= memfs_init,
	.getattr	= memfs_getattr,
	.readdir	= memfs_readdir,
	.open		= memfs_open,
	.read		= memfs_read
};


static Tcl_ThreadCreateType memfs_thread(ClientData cdata) //{{{
{
	int						startup_code = TCL_ERROR;
	int						sighandlers = 0;
	int						mounted = 0;
	struct fuse_session*	se = NULL;

	mountpoint = mkdtemp(template);
	if (mountpoint == NULL) {
		perror("Creating memfs mountpoint");
		goto done;
	}

	struct fuse_args args = {
		.argc = 1,
		.argv = (char*[]){"memfs"},
		.allocated = 0
	};

	/*
#define OPTION(t, p) {t, offsetof(struct options, p), 1}
	static cont struct fuse_opt option_spec[] = {
		FUSE_OPT_END
	};

	struct options {} options;
	if (-1 == fuse_opt_parse(&args, &options, option_spec, NULL)) goto done;
	*/

	fprintf(stderr, "Creating FUSE\n");
	fuse = fuse_new(&args, &memfs_ops, sizeof(memfs_ops), NULL);
	if (fuse == NULL) goto done;

	fprintf(stderr, "Creating FUSE sesion\n");
	se = fuse_get_session(fuse);
	if (se == NULL) goto done;

	fprintf(stderr, "Installing FUSE signal handlers\n");
	if (fuse_set_signal_handlers(se) != 0) goto done;
	sighandlers = 1;

	fprintf(stderr, "Mounting memfs at %s\n", mountpoint);
	if (fuse_mount(fuse, mountpoint) != 0) goto done;
	mounted = 1;

	startup_code = TCL_OK;

done:
	g_thread_startup_result = startup_code;
	g_thread_ready = 1;
	Tcl_MutexLock(&g_memfs_startup_mutex);
	fprintf(stderr, "Notifying main thread of startup\n");
	Tcl_ConditionNotify(&startup);
	Tcl_MutexUnlock(&g_memfs_startup_mutex);
	if (startup_code != TCL_OK) goto err;

	fprintf(stderr, "Entering FUSE session loop\n");
	const int loop_ret = fuse_loop(fuse);

	fprintf(stderr, "FUSE session loop returned: %d\n", loop_ret);

err:
	if (mounted && fuse) {
		fprintf(stderr, "Unmounting memfs from %s\n", mountpoint);
		fuse_unmount(fuse);
		mounted = 0;
	}

	if (sighandlers && se) {
		fprintf(stderr, "Removing FUSE signal handlers\n");
		fuse_remove_signal_handlers(se);
		sighandlers = 0;
	}

	if (fuse) {
		fprintf(stderr, "Destroying FUSE\n");
		fuse_destroy(fuse);
		fuse = NULL;
	}

	se = NULL;

	if (mountpoint) {
		fprintf(stderr, "Removing memfs mountpoint: %s\n", mountpoint);
		if (-1 == rmdir(mountpoint)) perror("Error removing memfs mountpoint");
		mountpoint = NULL;
	}

	fprintf(stderr, "Leaving memfs thread\n");
	TCL_THREAD_CREATE_RETURN;
}

//}}}

static int memfs_root(ClientData cdata, Tcl_Interp* interp, int objc, Tcl_Obj*const objv[]) //{{{
{
	int				code = TCL_OK;

	CHECK_ARGS(0, "");

	if (!mountpoint)
		THROW_ERROR_LABEL(done, code, "No mountpoint");

	Tcl_SetObjResult(interp, Tcl_NewStringObj(mountpoint, -1));

done:
	return code;
}

//}}}

int Memfs_Init(Tcl_Interp* interp) //{{{
{
	int							code = TCL_OK;

	Tcl_MutexLock(&g_memfs_init_mutex);

	if (!g_initialized) {
		Tcl_CreateThread(&g_memfs_tid, memfs_thread, NULL, TCL_THREAD_STACK_DEFAULT, TCL_THREAD_JOINABLE);
		Tcl_MutexLock(&g_memfs_startup_mutex);
		while (!g_thread_ready)
			Tcl_ConditionWait(&startup, &g_memfs_startup_mutex, NULL);
		Tcl_MutexUnlock(&g_memfs_startup_mutex);

		fprintf(stderr, "Got memfs thread startup result: %d\n", g_thread_startup_result);
		code = g_thread_startup_result;
		if (code != TCL_OK) goto done;

		g_initialized = 1;
	}

	Tcl_CreateObjCommand(interp, "::jitc::_memfs_root", memfs_root, NULL, NULL);

done:
	Tcl_MutexUnlock(&g_memfs_init_mutex);
	return code;
}

//}}}
int Memfs_Unload(Tcl_Interp* interp) //{{{
{
	int				code = TCL_OK;

	if (fuse) {
#if 0
		fprintf(stderr, "Calling fuse_exit\n");
		fuse_exit(fuse);
#else
		fprintf(stderr, "Calling umount(\"%s\")\n", mountpoint);
		const int rc = umount(mountpoint);
		if (rc == -1) perror("Error from umount");
		if (rc == -1) {
			Tcl_Obj* cmd = NULL;
			replace_tclobj(&cmd, Tcl_ObjPrintf("umount %s", mountpoint));
			const int sysrc = system(Tcl_GetString(cmd));
			fprintf(stderr, "Fell back to system(\"%s\")\n", Tcl_GetString(cmd));
			replace_tclobj(&cmd, NULL);
			if (sysrc != 0) {
				fprintf(stderr, "system umount rc: %d\n", sysrc);
			}
		}
#endif
	}

	int result;
	fprintf(stderr, "Joining memfs tid %p\n", g_memfs_tid);
	if (g_memfs_tid && TCL_OK != Tcl_JoinThread(g_memfs_tid, &result))
		fprintf(stderr, "Error joining g_memfs_tid: %d\n", result);

	Tcl_MutexFinalize(&g_memfs_init_mutex);
	g_memfs_init_mutex = NULL;
	Tcl_MutexFinalize(&g_memfs_startup_mutex);
	g_memfs_startup_mutex = NULL;
	Tcl_ConditionFinalize(&startup);

	return code;
}

//}}}
