#include <errno.h>
#include <alloca.h>

#include <esp_log.h>
#include <esp_vfs.h>

#include <esp32/rom/crc.h>

#include "packfs-priv.h"


static _lock_t pctxlock;
static pfs_ctx_t * pctx = NULL;

const char * packfs_mount = NULL;
const char * pprefix_path = NULL;

int pfs_newctx(void) {
	int fd = -1;

	_lock_acquire(&pctxlock);
	{
		for (int i=0; i<CONFIG_PACKFS_MAX_FILES; i++) {
			if (!pctx[i].inuse) {
				fd = i;
				memset(&pctx[i], 0, sizeof(pfs_ctx_t));
				pctx[i].inuse = true;
				break;
			}
		}
	}
	_lock_release(&pctxlock);
	return fd;
}

pfs_ctx_t * pfs_getctx(int fd) {
	if unlikely(fd < 0 || fd >= CONFIG_PACKFS_MAX_FILES) {
		return NULL;
	}

	pfs_ctx_t * c = &pctx[fd];
	return c->inuse? c : NULL;
}

bool pfs_checkinit(void) {
	return pctx != NULL;
}

bool pfs_checkheader(packfs_header_t * header) {
	// Check for magic
	if (header->magic != PACKFS_MAGIC) {
		//ESP_LOGW(PACKFS_TAG, "Bad magic number on pack file");
		return false;
	}

	// Sanity check index size
	if ((header->indexsize % sizeof(packfs_entry_t)) != 0) {
		//ESP_LOGW(PACKFS_TAG, "Bad section sizes on pack file");
		return false;
	}

	// Check CRC
	uint32_t calccrc = crc32_le(0, (void *)header, sizeof(packfs_header_t) - sizeof(uint32_t) - sizeof(packfs_hmac_t));
	if (calccrc != header->headercrc) {
		// TODO - remove this warning
		ESP_LOGW(PACKFS_TAG, "Bad header crc on pack file: reported=0x%x, calc=0x%x", header->headercrc, calccrc);
		return false;
	}

	return true;
}

bool pfs_readchunk(pfs_ctx_t * ctx, void * buffer, size_t length) {
	if (pfs_error(ctx) || fread(buffer, length, 1, ctx->backing) != 1) {
		pfs_error(ctx) = true;
		return false;
	}
	ctx->offset += length;
	return true;
}

bool pfs_seekabs(pfs_ctx_t * ctx, uint32_t offset) {
	if (pfs_error(ctx) || fseek(ctx->backing, offset, SEEK_SET) != 0) {
		pfs_error(ctx) = true;
		return false;
	}

	ctx->offset = offset;
	return true;
}

bool pfs_seekfwd(pfs_ctx_t * ctx, uint32_t length) {
	return pfs_seekabs(ctx, ctx->offset + length);
}

bool pfs_seekentry(pfs_ctx_t * ctx, packfs_entry_t * entry) {
	return pfs_seekabs(ctx, entry->offset);
}

bool pfs_readmeta(pfs_ctx_t * ctx, packfs_meta_t * meta, char * desc, uint8_t * value) {
	if (!pfs_readchunk(ctx, meta, sizeof(packfs_meta_t))) {
		return false;
	}

	// Read the description
	if (desc != NULL && !pfs_readchunk(ctx, desc, meta->descsize))		return false;
	else if (desc == NULL && !pfs_seekfwd(ctx, meta->descsize))			return false;

	// Read the value
	if (value != NULL && !pfs_readchunk(ctx, value, meta->valuesize))	return false;
	else if (value == NULL && !pfs_seekfwd(ctx, meta->valuesize))		return false;

	return true;
}

bool pfs_readindex(pfs_ctx_t * ctx, packfs_entry_t * entry) {
	return pfs_readchunk(ctx, entry, sizeof(packfs_entry_t));
}

bool pfs_findmeta(pfs_ctx_t * ctx, uint32_t metasize, const char * key, unsigned int * out_index) {
	packfs_meta_t meta;

	*out_index = 0;
	while (metasize > 0) {
		// Read the meta chunk
		if (!pfs_readmeta(ctx, &meta, NULL, NULL)) {
			return false;
		}

		// Check the key
		if (key != NULL && strcmp(key, meta.key) == 0) {
			return true;
		}

		metasize -= sizeof(packfs_meta_t) + meta.descsize + meta.valuesize;
		*out_index += 1;
	}

	return false;
}

bool pfs_findentry(pfs_ctx_t * ctx, uint32_t indexsize, const char * path, packfs_entry_t * out_entry) {
	uint16_t entries = indexsize / sizeof(packfs_entry_t);
	while (entries-- > 0) {
		if (!pfs_readindex(ctx, out_entry)) {
			return false;
		}

		if (strcmp(path, out_entry->path) == 0) {
			return true;
		}
	}

	return false;
}

bool pfs_prepentry(pfs_ctx_t * ctx) {
#ifdef CONFIG_PACKFS_LZO_SUPPORT
	if ((ctx->entry.flags & PF_LZO) && (!pfs_preplzo(ctx) || !pfs_readlzoheader(ctx))) {
		return false;
	}
#endif

	return true;
}

const char * pfs_parsepath(const char * fullpath, char * root, size_t rootlen) {
	labels(parseerr); // @suppress("Type cannot be resolved")

	char * subpath = strchr(fullpath, PACKFS_PATH_SEPERATOR);
	size_t rootsize = subpath != NULL? subpath - fullpath : strlen(fullpath);
	if (strlcpy(root, fullpath, rootsize) >= rootlen) {
		goto parseerr;
	}

	return subpath != NULL && subpath[1] != '\0'? subpath + 1 : NULL;

parseerr:
	root[0] = '\0';
	return NULL;

}

FILE * pfs_openbacking(const char * backingpath, uint32_t * length) {
	// Read file length if specified
	if (length != NULL) {
		struct stat st;
		memset(&st, 0, sizeof(struct stat));

		if (stat(backingpath, &st) != 0) {
			return NULL;
		}

		*length = st.st_size;
	}

	// Open spiffs file
	return fopen(backingpath, "r");
}

bool xfs_open(pfs_ctx_t * ctx, const char * backingpath, const char * subpath, uint32_t * out_length, packfs_header_t * out_header) {
	labels(openerr); // @suppress("Type cannot be resolved")

	// Allocate space
	if (out_length == NULL) out_length = alloca(sizeof(uint32_t));
	if (out_header == NULL) out_header = alloca(sizeof(packfs_header_t));

	// Open backing file
	if ((ctx->backing = pfs_openbacking(backingpath, out_length)) == NULL) {
		errnogoto(ENOENT, openerr);
	}

	// Read and check header
	if (!pfs_readchunk(ctx, out_header, sizeof(packfs_header_t)) || !pfs_checkheader(out_header)) {
		errnogoto(EFTYPE, openerr);
	}

	// Check version
	if (out_header->version != PACKFS_VERSION) {
		errnogoto(EPERM, openerr);
	}

	// Skip the meta section
	if (!pfs_seekfwd(ctx, out_header->metasize)) {
		errnogoto(EIO, openerr);
	}

	if (subpath != NULL) {
		if (!pfs_findentry(ctx, out_header->indexsize, subpath, &ctx->entry)) {
			// Entry not found
			errnogoto(ENOENT, openerr);
		}

		if ((ctx->entry.offset + ctx->entry.length) > *out_length) {
			// Entry is passed file bounds, pack file probably stripped
			errnogoto(ENOENT, openerr);
		}

		// Goto the start of entry and prep data fields
		if (!pfs_seekentry(ctx, &ctx->entry) || !pfs_prepentry(ctx)) {
			errnogoto(EIO, openerr);
		}
	}

	return true;

openerr:
	xfs_close(ctx);
	return false;
}

void xfs_close(pfs_ctx_t * ctx) {
	if (ctx->backing != NULL) {
		fclose(ctx->backing);
		ctx->backing = NULL;
	}

#ifdef CONFIG_PACKFS_LZO_SUPPORT
	// Free compression space
	pfs_lzofree(ctx);
#endif

	// Last, mark as not used
	ctx->inuse = false;
}

ssize_t pfs_write(int fd, const void * data, size_t size) {
	(void)fd;
	(void)data;
	(void)size;
	errno = ENOTSUP;
	return -1;
}

esp_err_t packfs_vfs_register(packfs_conf_t * config) {
	esp_err_t err = ESP_OK;

	// Sanity check system
	if (pfs_checkinit()) {
		return ESP_ERR_INVALID_STATE;
	}

	// Sanity check args
	if unlikely(config == NULL || config->base_path == NULL || config->prefix_path == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

#ifdef CONFIG_PACKFS_LZO_SUPPORT
	if (!pfs_initlzo()) {
		ESP_LOGE(PACKFS_TAG, "Failed to initialize lzo");
		return ESP_FAIL;
	}
#endif

	if ((pctx = calloc(CONFIG_PACKFS_MAX_FILES, sizeof(pfs_ctx_t))) == NULL) {
		return ESP_ERR_NO_MEM;
	}

	_lock_init(&pctxlock);
	packfs_mount = strdup(config->base_path);
	pprefix_path = strdup(config->prefix_path);

	// Check strdup success
	if (packfs_mount == NULL || pprefix_path == NULL) {
		free(pctx);
		pctx = NULL;
		return ESP_ERR_NO_MEM;
	}

	esp_vfs_t cb = {
		.flags = ESP_VFS_FLAG_DEFAULT,
		.open = pfs_open,
		.close = pfs_close,
		.read = pfs_read,
		.write = pfs_write,
		.lseek = pfs_lseek,
		.ioctl = pfs_ioctl,
		.fstat = pfs_fstat,
		.stat = pfs_stat,
		//.fcntl = pfs_fcntl,
		//.fsync = pfs_fsync,
		//.link = pfs_link,
		//.ulink = pfs_ulink,
		//.rename = pfs_rename,
		.opendir = pfs_opendir,
		.readdir = pfs_readdir,
		.readdir_r = pfs_readdir_r,
		.telldir = pfs_telldir,
		.seekdir = pfs_seekdir,
		.closedir = pfs_closedir,
		//.mkdir = pfs_mkdir,
		//.rmdir = pfs_rmdir,
		.access = pfs_access,
		//.truncate = pfs_truncate,
		//.utime = pfs_utime,
	};
	if ((err = esp_vfs_register(packfs_mount, &cb, NULL)) != ESP_OK) {
		ESP_LOGE(PACKFS_TAG, "Unable to register packfs vfs: err=%d", err);
		return err;
	}

	return err;
}
