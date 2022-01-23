#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <esp_err.h>
#include <esp_log.h>

#include "packfs-priv.h"

extern const char * pprefix_path;


int pfs_open(const char * path, int flags, int mode) {
	labels(openerr); // @suppress("Type cannot be resolved")

	// Sanity check args
	if unlikely(path == NULL) {
		errno = EINVAL;
		return -1;
	}

	//ESP_LOGI(PACKFS_TAG, "Open pack file: path=%s, flags=0x%x, mode=0x%x", path, flags, mode);
	// TODO - verify flags and mode

	// Get path within pack file
	/*char * subpath = strchr(path, PACKFS_PATH_SEPERATOR);
	if (subpath != NULL) {
		subpath += 1;
	}*/

	// Get free handle and ctx
	int fd = -1;
	pfs_ctx_t * ctx = NULL;
	if ((fd = pfs_newctx()) == -1 || (ctx = pfs_getctx(fd)) == NULL) {
		errnogoto(ENFILE, openerr);
	}

	// Get path within pack file
	char rootpath[PACKFS_MAX_FULLPATH] = {0};
	size_t prefixlen = strlcpy(rootpath, pprefix_path, sizeof(rootpath));
	const char * subpath = pfs_parsepath(path, &rootpath[prefixlen], sizeof(rootpath) - prefixlen);

	// Check for successful parse
	if (rootpath[0] == '\0') {
		errnogoto(ENOENT, openerr);
	}

	// Open the file
	if (!xfs_open(ctx, rootpath, subpath, NULL, NULL)) {
		goto openerr;
	}

	return fd;

	/*
	// Open spiffs file
	size_t pathlen = (subpath != NULL)? subpath - path - 1 : strlen(path);
	uint32_t length = 0;
	if ((ctx->spiffs_fp = pfs_openbacking(path, pathlen, &length)) == NULL) {
		errnogoto(ENOENT, openerr);
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

	if (subpath != NULL && subpath[0] != '\0') {
		// Skip the meta section
		if (!pfs_seekfwd(ctx, header.metasize)) {
			errnogoto(EIO, openerr);
		}

		if (!pfs_findentry(ctx, header.indexsize, subpath, &ctx->entry)) {
			// Entry not found
			errnogoto(ENOENT, openerr);
		}

		if ((ctx->entry.offset + ctx->entry.length) > length) {
			// Entry is passed file bounds, pack file probably stripped
			errnogoto(ENOENT, openerr);
		}

		// Goto the start of entry and prep data fields
		if (!pfs_seekentry(ctx, &ctx->entry) || !pfs_prepentry(ctx)) {
			errnogoto(EIO, openerr);
		}

	} else {
		// No path within pack file specified
		memset(&ctx->entry, 0, sizeof(packfs_entry_t));
	}

	return fd;
	*/

openerr:
	pfs_close(fd);
	return -1;
}

int pfs_close(int fd) {
	pfs_ctx_t * ctx = pfs_getctx(fd);
	if unlikely(ctx == NULL) return -1;

	xfs_close(ctx);
	return 0;
}

static ssize_t pfs_readreg(pfs_ctx_t * ctx, void * buffer, size_t length) {
	labels(readerr); // @suppress("Type cannot be resolved")

	// Prevent file overrun
	length = min(length, ctx->entry.offset + ctx->entry.length - ctx->offset);
	if (length == 0) {
		// EOF
		return 0;
	}

	// Read the chunk
	if (!pfs_readchunk(ctx, buffer, length)) {
		errnogoto(EIO, readerr);
	}

	return length;

readerr:
	return -1;
}

ssize_t pfs_read(int fd, void * buffer, size_t length) {
	pfs_ctx_t * ctx = pfs_getctx(fd);

	// Check arguments
	if unlikely(ctx == NULL || buffer == NULL) {
		errno = EINVAL;
		return -1;
	}

	return xfs_read(ctx, buffer, length);
}

static off_t pfs_seekreg(pfs_ctx_t * ctx, off_t offset, int mode) {

	// Make offset referenced from start of entry
	if (mode == SEEK_CUR) {
		offset += ctx->offset - ctx->entry.offset;
	} else if (mode == SEEK_END) {
		offset += ctx->entry.length;
	}

	// Ignore the image hash header
	off_t extra = 0;
	if (ctx->entry.flags & PT_IMG) {
		extra += PACKFS_HASHSIZE;
	}

	// Make sure we're within file size
	if (offset < 0 || offset > ctx->entry.length) {
		errno = EOVERFLOW;
		return -1;
	}

	uint32_t fulloffset = ctx->entry.offset + offset + extra;
	if (ctx->offset != fulloffset && !pfs_seekabs(ctx, fulloffset)) {
		errno = EIO;
		return -1;
	}

	return offset;
}

off_t pfs_lseek(int fd, off_t offset, int mode) {
	pfs_ctx_t * ctx = pfs_getctx(fd);

	// Check arguments
	if unlikely(ctx == NULL) {
		errno = EINVAL;
		return -1;
	}

	return xfs_lseek(ctx, offset, mode);
}

int pfs_ioctl(int fd, int cmd, va_list args) {
	pfs_ctx_t * ctx = pfs_getctx(fd);

	// Check arguments
	if unlikely(ctx == NULL) {
		errno = EINVAL;
		return -1;
	}

	return xfs_ioctl(ctx, cmd, args);
}

ssize_t xfs_read(pfs_ctx_t * ctx, void * buffer, size_t length) {

	// Check if errored
	if unlikely(pfs_error(ctx)) {
		errno = EBADF;
		return -1;
	}

	//ESP_LOGI(PACKFS_TAG, "Reading data from file: path=%s, length=%d", ctx->entry.path, length);

	if (ctx->entry.flags & PF_LZO) {
		// Compressed file
#ifdef CONFIG_PACKFS_LZO_SUPPORT
		return pfs_readlzo(ctx, buffer, length);
#else
		errno = EPROTO;
		return -1;
#endif
	} else {
		// Regular file, normal read
		return pfs_readreg(ctx, buffer, length);
	}
}

off_t xfs_lseek(pfs_ctx_t * ctx, off_t offset, int mode) {
	// Check if errored
	if unlikely(pfs_error(ctx)) {
		errno = EBADF;
		return -1;
	}

	//ESP_LOGI(PACKFS_TAG, "Seeking in file: path=%s, offset=%ld, mode=%d", ctx->entry.path, offset, mode);

	if (ctx->entry.flags & PF_LZO) {
		// Compressed file
#ifdef CONFIG_PACKFS_LZO_SUPPORT
		return pfs_seeklzo(ctx, offset, mode);
#else
		errno = EPROTO;
		return -1;
#endif
	} else {
		// Regular file, normal seek
		return pfs_seekreg(ctx, offset, mode);
	}
}

int xfs_ioctl(pfs_ctx_t * ctx, int cmd, va_list args) {
	labels(ioctlerr); // @suppress("Type cannot be resolved")

	// Check if errored
	if unlikely(pfs_error(ctx)) {
		errno = EBADF;
		return -1;
	}

	int ret = -1;
	uint32_t offset = ctx->offset;
	//ESP_LOGI(PACKFS_TAG, "IOCtl on file: path=%s, cmd=%d", ctx->entry.path, cmd);

	switch (cmd) {
		case PIOCTL_METASIZE:
		case PIOCTL_METAFINDINDEX:
		case PIOCTL_METAFINDNAME:

		case PIOCTL_ENTRYSIZE:
		case PIOCTL_ENTRYFINDINDEX:
		case PIOCTL_ENTRYFINDPATH: {
			// Read meta size
			packfs_header_t header;
			if (!pfs_seekabs(ctx, 0) || !pfs_readchunk(ctx, &header, sizeof(packfs_header_t))) {
				errnogoto(EIO, ioctlerr);
			}

			switch (cmd) {
				case PIOCTL_METASIZE: {
					// Read args
					int * out_size = va_arg(args, int *);

					// Sanity check args
					if (out_size == NULL) {
						errnogoto(EINVAL, ioctlerr);
					}

					*out_size = header.metasize / sizeof(packfs_meta_t);
					ret = 0;
					break;
				}
				case PIOCTL_METAFINDINDEX: {
					// Read args
					int in_index = va_arg(args, int);
					packfs_meta_t * out_meta = va_arg(args, packfs_meta_t *);

					// Sanity check args
					if (in_index < 0 || in_index > (header.metasize / sizeof(packfs_meta_t)) || out_meta == NULL) {
						errnogoto(EINVAL, ioctlerr);
					}

					// Read in meta
					if (!pfs_seekfwd(ctx, in_index * sizeof(packfs_meta_t)) || !pfs_readmeta(ctx, out_meta)) {
						errnogoto(EIO, ioctlerr);
					}
					ret = 0;
					break;
				}
				case PIOCTL_METAFINDNAME: {
					// Read args
					const char * in_key = va_arg(args, const char *);
					packfs_meta_t * out_meta = va_arg(args, packfs_meta_t *);

					// Sanity check args
					if (in_key == NULL || strlen(in_key) > (PACKFS_MAX_METAKEY - 1) || out_meta == NULL) {
						errnogoto(EINVAL, ioctlerr);
					}

					// Find meta by name
					ret = pfs_findmeta(ctx, header.metasize, in_key, out_meta)? 1 : 0;
					break;
				}

				case PIOCTL_ENTRYSIZE: {
					// Read args
					int * out_size = va_arg(args, int *);

					// Sanity check args
					if (out_size == NULL) {
						errnogoto(EINVAL, ioctlerr);
					}

					*out_size = header.indexsize / sizeof(packfs_entry_t);
					ret = 0;
					break;
				}
				case PIOCTL_ENTRYFINDINDEX: {
					// Read args
					int in_index = va_arg(args, int);
					packfs_entry_t * out_entry = va_arg(args, packfs_entry_t *);

					// Sanity check args
					if (in_index < 0 || in_index > (header.indexsize / sizeof(packfs_entry_t)) || out_entry == NULL) {
						errnogoto(EINVAL, ioctlerr);
					}

					// Read in meta
					if (!pfs_seekfwd(ctx, header.metasize + in_index * sizeof(packfs_entry_t)) || !pfs_readentry(ctx, out_entry)) {
						errnogoto(EIO, ioctlerr);
					}
					ret = 0;
					break;
				}
				case PIOCTL_ENTRYFINDPATH: {
					// Read args
					const char * in_path = va_arg(args, const char *);
					packfs_entry_t * out_entry = va_arg(args, packfs_entry_t *);

					// Sanity check args
					if (in_path == NULL || strlen(in_path) > (PACKFS_MAX_ENTRYPATH - 1) || out_entry == NULL) {
						errnogoto(EINVAL, ioctlerr);
					}

					// Seek to index
					if (!pfs_seekfwd(ctx, header.metasize)) {
						errnogoto(EIO, ioctlerr);
					}

					// Find meta by name
					ret = pfs_findentry(ctx, header.metasize, in_path, out_entry)? 1 : 0;
					break;
				}
			}
			break;
		}

		case PIOCTL_CURRENTENTRY: {
			// Read args
			packfs_entry_t * out_entry = va_arg(args, packfs_entry_t *);

			// Sanity check args
			if (out_entry == NULL) {
				errnogoto(EINVAL, ioctlerr);
			}

			// Copy entry over
			memcpy(out_entry, &ctx->entry, sizeof(packfs_entry_t));

			ret = 0;
			break;
		}
		case PIOCTL_CURRENTIMGHASH: {
			// Read args
			char * out_hash = va_arg(args, char *);

			// Sanity check args
			if (out_hash == NULL) {
				errnogoto(EINVAL, ioctlerr);
			}

			// Only available on img entry types
			if (!(ctx->entry.flags & PT_IMG)) {
				ret = 0;
				break;
			}

			// Read out hash
			if (!pfs_seekentry(ctx, &ctx->entry) || !pfs_readimghash(ctx, (uint8_t *)out_hash)) {
				errnogoto(EIO, ioctlerr);
			}

			ret = 1;
			break;
		}
		default: {
			errnogoto(EINVAL, ioctlerr);
		}
	}

ioctlerr:
	// Restore the offset
	pfs_seekabs(ctx, offset);
	return ret;
}
