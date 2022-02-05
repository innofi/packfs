#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include "packfs-priv.h"
#include "imagefs-priv.h"


#ifndef CONFIG_IMAGEFS_SUPPORT
#error "This file should NOT be included if CONFIG_PACKFS_IMAGEFS_SUPPORT is not set."
#else

extern char imagefs_path[PACKFS_MAX_FULLPATH];

int ifs_open(const char * path, int flags, int mode) {
	labels(openerr); // @suppress("Type cannot be resolved")

	// Sanity check args
	if (path == NULL || path[0] == '\0') {
		errno = EINVAL;
		return -1;
	}

	//ESP_LOGI(IMAGEFS_TAG, "Open imagefs file: path=%s", path);
	// TODO - verify flags and mode

	// Get free handle and ctx
	int fd = -1;
	ifs_ctx_t * ictx = NULL;
	if ((fd = ifs_newctx()) == -1 || (ictx = ifs_getctx(fd)) == NULL) {
		errnogoto(ENFILE, openerr);
	}

	if (memcmp(path, IMAGEFS_PATH_META, strlen(IMAGEFS_PATH_META)) == 0) {
		// We're opening a meta object
		const char * key = &path[strlen(IMAGEFS_PATH_META)];

		// Open the file
		if (!xfs_open(&ictx->pctx, imagefs_path, NULL, NULL, NULL)) {
			errnogoto(EIO, openerr);
		}

		// Find the meta key
		int xfs_ioctl_proxy(pfs_ctx_t * ctx, int cmd, ...) {
			va_list ap;
			va_start(ap, cmd);
			int r = xfs_ioctl(ctx, cmd, ap);
			va_end(ap);
			return r;
		}
		if (xfs_ioctl_proxy(&ictx->pctx, PIOCTL_METAFIND, key, &ictx->pctx.meta) != 1) {
			errnogoto(ENOENT, openerr);
		}

		// Configure the ictx
		ictx->mode = IM_READMETA;
		ictx->offset = 0;

		return fd;

	} else {
		// We're opening an indexed entry
		if (!xfs_open(&ictx->pctx, imagefs_path, path, NULL, NULL)) {
			goto openerr;
		}

		// Configure the ictx
		ictx->mode = IM_OPENENTRY;
		return fd;
	}

openerr:
		ifs_close(fd);
		return -1;
}

int ifs_close(int fd) {
	ifs_ctx_t * ictx = ifs_getctx(fd);
	if unlikely(ictx == NULL) return -1;

	xfs_close(&ictx->pctx);
	return 0;
}

ssize_t ifs_read(int fd, void * buffer, size_t length) {
	ifs_ctx_t * ictx = ifs_getctx(fd);

	// Check arguments
	if unlikely(ictx == NULL || buffer == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Check if errored
	if unlikely(pfs_error(&ictx->pctx)) {
		errno = EBADF;
		return -1;
	}

	switch (ictx->mode) {
		case IM_READMETA: {
			length = min(length, sizeof(packfs_meta_t) - ictx->offset);
			if (length == 0) {
				// EOF
				return 0;
			}
			memcpy(buffer, &((uint8_t *)&ictx->pctx.meta)[ictx->offset], length);
			ictx->offset += length;
			return length;
		}
		case IM_OPENENTRY: {
			return xfs_read(&ictx->pctx, buffer, length);
		}
		default: {
			errno = EIO;
			return -1;
		}
	}
}

off_t ifs_lseek(int fd, off_t offset, int mode) {
	ifs_ctx_t * ictx = ifs_getctx(fd);

	// Check arguments
	if unlikely(ictx == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Check if errored
	if unlikely(pfs_error(&ictx->pctx)) {
		errno = EBADF;
		return -1;
	}

	switch (ictx->mode) {
		case IM_READMETA: {
			// Make offset referenced from start of entry
			if (mode == SEEK_CUR) {
				offset += ictx->offset;
			} else if (mode == SEEK_END) {
				offset += sizeof(packfs_meta_t);
			}

			// Sanity check offset
			if (offset < 0 || offset > sizeof(packfs_meta_t)) {
				errno = EOVERFLOW;
				return -1;
			}

			return ictx->offset = offset;
		}
		case IM_OPENENTRY: {
			return xfs_lseek(&ictx->pctx, offset, mode);
		}
		default: {
			errno = EIO;
			return -1;
		}
	}
}

int ifs_ioctl(int fd, int cmd, va_list args) {
	ifs_ctx_t * ictx = ifs_getctx(fd);

	// Check arguments
	if unlikely(ictx == NULL) {
		errno = EINVAL;
		return -1;
	}

	switch (cmd) {
		case PIOCTL_CURRENTENTRY:
		case PIOCTL_CURRENTIMGHASH: {
			if (ictx->mode != IM_OPENENTRY) {
				// These ioctls are only allowed on IR_ENTRYDATA
				errno = EINVAL;
				return -1;
			}
		}
	}

	return xfs_ioctl(&ictx->pctx, cmd, args);
}

int ifs_fstat(int fd, struct stat * st) {
	ifs_ctx_t * ictx = ifs_getctx(fd);

	// Check arguments
	if unlikely(ictx == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Check if errored
	if unlikely(pfs_error(&ictx->pctx)) {
		errno = EBADF;
		return -1;
	}

	switch (ictx->mode) {
		case IM_READMETA: {
			if (st != NULL) {
				memset(st, 0, sizeof(struct stat));
				st->st_mode = S_IRWXU | S_IRWXG | S_IRWXO | S_IFREG;
				st->st_mtime = st->st_atime = st->st_ctime = 0;
				st->st_size = sizeof(packfs_meta_t);
				st->st_blksize = 1;
				st->st_blocks = st->st_size;
			}
			return 0;
		}
		case IM_OPENENTRY: {
			return xfs_fstat(&ictx->pctx, st);
		}
		default: {
			errno = EIO;
			return -1;
		}
	}
}

int ifs_stat(const char * path, struct stat * st) {
	int fd = ifs_open(path, O_RDONLY, 0);
	if (fd == -1) {
		return -1;
	}

	int save_ret = ifs_fstat(fd, st);
	error_t save_errno = errno;
	ifs_close(fd);

	errno = save_errno;
	return save_ret;
}

int ifs_access(const char * path, int amode) {
	if unlikely(path == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (amode != F_OK && amode != R_OK) {
		errno = EACCES;
		return -1;
	}

	return ifs_stat(path, NULL);
}


DIR * ifs_opendir(const char * path) {
	labels(openerr); // @suppress("Type cannot be resolved")

	//ESP_LOGI(IMAGEFS_TAG, "Opendir pack file: path=%s", fullpath);

	// Get free handle and ctx
	int fd = -1;
	ifs_ctx_t * ictx = NULL;
	if ((fd = ifs_newctx()) == -1 || (ictx = ifs_getctx(fd)) == NULL) {
		errno = ENFILE;
		return NULL;
	}

	// Allocate dir entry
	// TODO - match path (not just root imagefs_path)
	pfs_dirent_t * dir = NULL;
	if ((dir = xfs_opendir(&ictx->pctx, imagefs_path, fd)) == NULL) {
		ifs_close(fd);
		return NULL;
	}

	// Rewind to beginning of meta section
	if (!pfs_seekabs(&ictx->pctx, sizeof(packfs_header_t))) {
		errnogoto(EIO, openerr);
	}

	return (DIR *)dir;

openerr:
	ifs_closedir((DIR *)dir);
	return NULL;
}

int ifs_closedir(DIR * pdir) {
	// Check parameters
	if unlikely(pdir == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Unpack context
	pfs_dirent_t * dir = (pfs_dirent_t *)pdir;

	// Free the resources
	ifs_close(dir->fd);
	free(dir);

	return 0;
}

int ifs_readdir_r(DIR * pdir, struct dirent * entry, struct dirent ** out) {
	// Unpack context
	pfs_dirent_t * dir = (pfs_dirent_t *)pdir;
	ifs_ctx_t * ictx = NULL;

	// Check parameters
	if unlikely(pdir == NULL || (ictx = ifs_getctx(dir->fd)) == NULL) {
		return errno = EINVAL;
	}

	// Determine if we're within meta section
	if (ictx->pctx.offset >= sizeof(packfs_header_t) && ictx->pctx.offset < dir->index_start) {
		// TODO - remove this functionality

		// Read next meta
		if (!pfs_readmeta(&ictx->pctx, &ictx->pctx.meta)) {
			return errno = EIO;
		}

		entry->d_ino = 0;
		entry->d_type = DT_REG;
		strlcpy(entry->d_name, IMAGEFS_PATH_META, sizeof(entry->d_name));
		strlcat(entry->d_name, ictx->pctx.meta.key, sizeof(entry->d_name));

		*out = entry;
		return 0;
	}

	// Handle index section
	return xfs_readdir_r(&ictx->pctx, dir, entry, out);
}

struct dirent * ifs_readdir(DIR * pdir) {
	// Check parameters
	if unlikely(pdir == NULL) {
		errno = EINVAL;
		return NULL;
	}

	// Unpack context
    pfs_dirent_t * dir = (pfs_dirent_t *)pdir;

    // Forward to readdir_r
	int err;
	struct dirent * out;
	if ((err = ifs_readdir_r(pdir, &dir->ent, &out)) != 0) {
		errno = err;
		return NULL;
	}

	return out;
}


long ifs_telldir(DIR * pdir) {
	// Unpack context
	pfs_dirent_t * dir = (pfs_dirent_t *)pdir;
	ifs_ctx_t * ictx = NULL;

	// Check parameters
	if unlikely(pdir == NULL || (ictx = ifs_getctx(dir->fd)) == NULL || ictx->pctx.offset < sizeof(packfs_header_t)) {
		errno = EINVAL;
		return 0;
	}

	if (ictx->pctx.offset < dir->index_start) {
		return (ictx->pctx.offset - sizeof(packfs_header_t)) / sizeof(packfs_meta_t);

	} else {
		return (dir->index_start - sizeof(packfs_header_t)) / sizeof(packfs_meta_t) + xfs_telldir(&ictx->pctx, dir);
	}
}

void ifs_seekdir(DIR * pdir, long offset) {
	// Unpack context
	pfs_dirent_t * dir = (pfs_dirent_t *)pdir;
	ifs_ctx_t * ictx = NULL;

	// Check parameters
	if unlikely(pdir == NULL || offset < 0 || (ictx = ifs_getctx(dir->fd)) == NULL) {
		errno = EINVAL;
		return;
	}

	uint32_t nummetas = (dir->index_start - sizeof(packfs_header_t)) / sizeof(packfs_meta_t);
	uint32_t numentries = dir->index_length / sizeof(packfs_entry_t);

	// Sanity check offset
	if (offset > (nummetas + numentries)) {
		errno = EINVAL;
		return;
	}

	// Seek to offset
	size_t bytesoffset = offset < nummetas? offset * sizeof(packfs_meta_t) : nummetas * sizeof(packfs_meta_t) + (offset - nummetas) * sizeof(packfs_entry_t);
	if (!pfs_seekabs(&ictx->pctx, sizeof(packfs_header_t) + bytesoffset)) {
		errno = EIO;
		return;
	}
}

#endif
