#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>

#include "packfs-priv.h"

int pfs_fstat(int fd, struct stat * st) {
	pfs_ctx_t * ctx = pfs_getctx(fd);

	// Check arguments
	if unlikely(ctx == NULL) {
		errno = EINVAL;
		return -1;
	}

	return xfs_fstat(ctx, st);
}

int pfs_stat(const char * path, struct stat * st) {
	int fd = pfs_open(path, O_RDONLY, 0);
	if (fd == -1) {
		return -1;
	}

	int save_ret = pfs_fstat(fd, st);
	error_t save_errno = errno;
	pfs_close(fd);

	errno = save_errno;
	return save_ret;
}

int pfs_access(const char * path, int amode) {
	if unlikely(path == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (amode != F_OK && amode != R_OK) {
		errno = EACCES;
		return -1;
	}

	return pfs_stat(path, NULL);
}

int xfs_fstat(pfs_ctx_t * ctx, struct stat * st) {
	// Check if errored
	if unlikely(pfs_error(ctx)) {
		errno = EBADF;
		return -1;
	}

	//ESP_LOGI(PACKFS_TAG, "Stat file: path=%s", ctx->entry.path);

	if (st != NULL) {
		memset(st, 0, sizeof(struct stat));
		st->st_mode = S_IRUSR | S_IRGRP | S_IROTH | S_IFREG;
		st->st_mtime = st->st_atime = st->st_ctime = 0;

		if (ctx->entry.flags & PF_LZO) {
			// Compressed file
#ifdef CONFIG_PACKFS_LZO_SUPPORT
			st->st_size = ctx->lzo.header.uncompressed_length;
			st->st_blksize = ctx->lzo.header.blocksize;
			st->st_blocks = (ctx->lzo.header.uncompressed_length + ctx->lzo.header.blocksize - 1) / ctx->lzo.header.blocksize;
#else
			st->st_size = 0;
			st->st_blksize = 1;
			st->st_blocks = 0;
#endif
		} else {
			// Regular file
			st->st_size = ctx->entry.length - (ctx->entry.flags & PT_IMG)? PACKFS_HASHSIZE : 0;
			st->st_blksize = 1;
			st->st_blocks = st->st_size;
		}
	}

	return 0;
}
