#include <errno.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>

#include "packfs-priv.h"

DIR * pfs_opendir(const char * path) {
	if unlikely(path == NULL) {
		errno = EINVAL;
		return NULL;
	}

	//ESP_LOGI(PACKFS_TAG, "Opendir pack file: path=%s", path);

	// Get free handle and ctx
	int fd = -1;
	pfs_ctx_t * ctx = NULL;
	if ((fd = pfs_newctx()) == -1 || (ctx = pfs_getctx(fd)) == NULL) {
		errno = ENFILE;
		return NULL;
	}

	// Allocate dir entry
	pfs_dirent_t * dir = NULL;
	if ((dir = xfs_opendir(ctx, path, fd)) == NULL) {
		pfs_close(fd);
		return NULL;
	}

	return (DIR *)dir;
	/*
	// Determine basepath
	char * subpath = strchr(path, PACKFS_PATH_SEPERATOR);
	size_t pathlen = subpath == NULL? strlen(path) : subpath - path;

	// Open spiffs file
	uint32_t length = 0;
	if ((ctx->spiffs_fp = pfs_openbacking(path, pathlen, &length)) == NULL) {
		errnogoto(ENOTDIR, openerr);
	}

	// Read and check header
	packfs_header_t header;
	if (!pfs_readchunk(ctx, &header, sizeof(packfs_header_t)) || !pfs_checkheader(&header)) {
		errnogoto(EFTYPE, openerr);
	}

	// Check version
	if (header.version != PACKFS_VERSION) {
		errnogoto(EPERM, openerr);
	}

	// Skip the meta section
	if (!pfs_seekfwd(ctx, header.metasize)) {
		errnogoto(EIO, openerr);
	}
	*/



	//return (DIR *)dir;


}

int pfs_closedir(DIR * pdir) {
	// Check parameters
	if unlikely(pdir == NULL) {
		errno = EINVAL;
		return -1;
	}

	// Unpack context
	pfs_dirent_t * dir = (pfs_dirent_t *)pdir;

	// Free the resources
	pfs_close(dir->fd);
	free(dir);

	return 0;
}

int pfs_readdir_r(DIR * pdir, struct dirent * entry, struct dirent ** out) {
	// Unpack context
	pfs_dirent_t * dir = (pfs_dirent_t *)pdir;
	pfs_ctx_t * ctx = NULL;

	// Check parameters
	if unlikely(pdir == NULL || (ctx = pfs_getctx(dir->fd)) == NULL) {
		return errno = EINVAL;
	}

	return xfs_readdir_r(ctx, dir, entry, out);
}

struct dirent * pfs_readdir(DIR * pdir) {
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
	if ((err = pfs_readdir_r(pdir, &dir->ent, &out)) != 0) {
		errno = err;
		return NULL;
	}

	return out;
}

long pfs_telldir(DIR * pdir) {
	// Unpack context
	pfs_dirent_t * dir = (pfs_dirent_t *)pdir;
	pfs_ctx_t * ctx = NULL;

	// Check parameters
	if unlikely(pdir == NULL || (ctx = pfs_getctx(dir->fd)) == NULL || ctx->offset < dir->index_start) {
		errno = EINVAL;
		return 0;
	}

	return xfs_telldir(ctx, dir);
}

void pfs_seekdir(DIR * pdir, long offset) {
	// Unpack context
	pfs_dirent_t * dir = (pfs_dirent_t *)pdir;
	pfs_ctx_t * ctx = NULL;

	// Check parameters
	if unlikely(pdir == NULL || offset < 0 || (ctx = pfs_getctx(dir->fd)) == NULL) {
		errno = EINVAL;
		return;
	}

	// Sanity check offset
	uint32_t bytesoffset = offset * sizeof(packfs_entry_t);
	if (bytesoffset > dir->index_length) {
		errno = EINVAL;
		return;
	}

	// Seek to offset
	if (!pfs_seekabs(ctx, dir->index_start + bytesoffset)) {
		errno = EIO;
		return;
	}
}

pfs_dirent_t * xfs_opendir(pfs_ctx_t * ctx, const char * path, int fd) {
	labels(openerr); // @suppress("Type cannot be resolved")

	// Allocate the dir context
	pfs_dirent_t * dir = calloc(1, sizeof(pfs_dirent_t));
	if (dir == NULL) {
		errno = ENOMEM;
		return NULL;
	}

	// Open the file
	packfs_header_t header;
	if (!xfs_open(ctx, path, NULL, &dir->file_length, &header)) {
		errnogoto(ENOTDIR, openerr);
	}

	// Setup the index offsets
	dir->fd = fd;
	dir->index_start = ctx->offset;
	dir->index_length = header.indexsize;

	return dir;

openerr:
	free(dir);
	return NULL;
}

int xfs_readdir_r(pfs_ctx_t * ctx, pfs_dirent_t * dir, struct dirent * entry, struct dirent ** out) {

	// Check to see of we're out of files
	if (ctx->offset < dir->index_start || ctx->offset >= (dir->index_start + dir->index_length)) {
		*out = NULL;
		return 0;
	}

	// Read next entry index
	if (!pfs_readentry(ctx, &ctx->entry)) {
		return errno = EIO;
	}

	// Ensure entry bounds are within file bounds
	if ((ctx->entry.offset + ctx->entry.length) > dir->file_length) {
		// Entry is passed file bounds, pack file probably stripped
		*out = NULL;
		return 0;
	}

	entry->d_ino = 0;
	entry->d_type = DT_REG;
	strlcpy(entry->d_name, ctx->entry.path, sizeof(entry->d_name));

	*out = entry;
	return 0;
}

long xfs_telldir(pfs_ctx_t * ctx, pfs_dirent_t * dir) {
	return (ctx->offset - dir->index_start) / sizeof(packfs_entry_t);
}
