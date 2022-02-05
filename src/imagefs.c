#include <errno.h>

#include <esp_log.h>
#include <esp_vfs.h>
#include <esp_ota_ops.h>

#include "packfs-priv.h"
#include "imagefs-priv.h"


#if !defined(CONFIG_IMAGEFS_SUPPORT)
#error "This file should NOT be included if CONFIG_PACKFS_IMAGEFS_SUPPORT is not set."
#elif !defined(CONFIG_PACKFS_PROCESS_SUPPORT) || !defined(CONFIG_PACKFS_STREAM_SUPPORT)
#error "Necessary dependencies not enabled. Needs CONFIG_PACKFS_PROCESS_SUPPORT and CONFIG_PACKFS_PROCESS_FROMFILE_SUPPORT."
#endif

extern const char * pprefix_path;

static _lock_t ictxlock;
static ifs_ctx_t * ictx = NULL;

imagefs_filename_t ifilename = {NULL, NULL, NULL};
char imagefs_path[PACKFS_MAX_FULLPATH] = {0};
const char * imagefs_mount = NULL;
const char * iprefix_path = NULL;


int ifs_newctx(void) {
	int fd = -1;

	_lock_acquire(&ictxlock);
	{
		for (int i=0; i<CONFIG_PACKFS_MAX_FILES; i++) {
			if (!ictx[i].pctx.inuse) {
				fd = i;
				memset(&ictx[i], 0, sizeof(ifs_ctx_t));
				ictx[i].pctx.inuse = true;
				break;
			}
		}
	}
	_lock_release(&ictxlock);
	return fd;
}

ifs_ctx_t * ifs_getctx(int fd) {
	return (fd < 0 || fd >= CONFIG_PACKFS_MAX_FILES)? NULL : &ictx[fd];
}

bool ifs_checkinit() {
	return iprefix_path != NULL && ifilename.namegen != NULL;
}

bool ifs_imagepath(const esp_app_desc_t * app, char * path, size_t pathlen) {
	if (snprintf(path, pathlen, "%s/", iprefix_path) >= pathlen) {
		return false;
	}

	size_t prefixlen = strlen(path);
	return ifilename.namegen(&path[prefixlen], pathlen - prefixlen, app->project_name, app->version);
}

bool ifs_scratchpath(char * path, size_t pathlen) {
	if (snprintf(path, pathlen, "%s/", iprefix_path) >= pathlen) {
		return false;
	}

	size_t prefixlen = strlen(path);
	if (ifilename.scratchfile != NULL) {
		return ifilename.scratchfile(&path[pathlen], pathlen - prefixlen);
	} else {
		return ifilename.namegen(&path[pathlen], pathlen - prefixlen, "scratch", "0");
	}
}

static bool ifs_default_filenamegen(char * filenamebuf, size_t buflen, const char * projname, const char * projversion) {
	return snprintf(filenamebuf, buflen, "image-%s-v%s.pack", projname, projversion) < buflen;
}

static bool ifs_default_filenamecheck(const char * filename) {
	size_t len = strlen(filename);
	return len > 12 && memcmp(filename, "image-", 6) == 0 && memcmp(&filename[len - 5], ".pack", 5) == 0;
}

bool ifs_default_scratchfile(char * path, size_t pathlen) {
	if (pathlen < 22) return false;
	strcpy(path, "image-scratchfile.pack");
	return true;
}

static void ifs_verify_onerror(void * ud, const char * file, unsigned int line, packfs_proc_section_t section, int err) {
	ESP_LOGE(IMAGEFS_TAG, "Validation process error in section %d: file=%s, line=%u, err=%d", section, file, line, err);
	*(bool *)ud = false;
}

static bool ifs_verify_onbodyhash(void * ud, uint8_t * reported_hash, uint8_t * calculated_hash, bool hash_matches) {
	bool * valid = ud;
	*valid = *valid && hash_matches;
	return hash_matches;
}

static bool ifs_verify_onentrystart(void * ud, const packfs_entry_t * entry, uint32_t filesize) {
#ifdef CONFIG_PACKFS_IMAGEFS_VERBOSEINIT
	ESP_LOGI(IMAGEFS_TAG, "Found %s file in image pack: %s %s size=%u", (entry->flags & PFT_REG)? "regular" : (entry->flags & PFT_IMG)? "image" : "UNKNOWN", entry->path, (entry->flags & PF_LZO)? "compressed" : "uncompressed", filesize);
#endif
	return entry->flags & PFT_IMG;
}

static bool ifs_verify_onimgentryend(void * ud, const packfs_entry_t * entry, uint8_t * reported_hash, uint8_t * calculated_hash, bool hash_matches) {
	bool * valid = ud;
	*valid = *valid && hash_matches;
	return hash_matches;
}

esp_err_t imagefs_filename_register(const char * prefix_path, imagefs_filename_t * filename_funcs) {
	if (prefix_path == NULL) {
		// Use packfs prefix_path if prefix_path not specified
		prefix_path = pprefix_path;
	}

	// Sanity check
	if unlikely(prefix_path == NULL) {
		// No prefix path, error
		return ESP_ERR_INVALID_ARG;
	}

	if ((iprefix_path = strdup(prefix_path)) == NULL) {
		return ESP_ERR_NO_MEM;
	}

	if (filename_funcs != NULL && filename_funcs->namegen != NULL) {
		ifilename.namegen = filename_funcs->namegen;
		ifilename.namecheck = filename_funcs->namecheck;
		ifilename.scratchfile = filename_funcs->scratchfile;

	} else {
		// Use default functions
		ifilename = (imagefs_filename_t){
			.namegen = ifs_default_filenamegen,
			.namecheck = ifs_default_filenamecheck,
			.scratchfile = ifs_default_scratchfile
		};
	}

	return ESP_OK;
}

esp_err_t imagefs_vfs_register(imagefs_conf_t * config) {
	esp_err_t err = ESP_OK;

	// Sanity check args
	if unlikely(config == NULL || config->base_path == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	if ((ictx = calloc(CONFIG_PACKFS_MAX_FILES, sizeof(ifs_ctx_t))) == NULL) {
		return ESP_ERR_NO_MEM;
	}

	_lock_init(&ictxlock);
	imagefs_mount = strdup(config->base_path);

	// Check strdup success
	if (imagefs_mount == NULL || imagefs_filename_register(config->prefix_path, &config->filename) != ESP_OK) {
		free(ictx);
		ictx = NULL;
		return ESP_ERR_NO_MEM;
	}

	// Determine image path
	if (!ifs_imagepath(esp_ota_get_app_description(), imagefs_path, sizeof(imagefs_path))) {
		return ESP_FAIL;
	}

#ifdef CONFIG_PACKFS_IMAGEFS_VERBOSEINIT
	ESP_LOGI(IMAGEFS_TAG, "Using image file %s", imagefs_path);
#endif

	// Verify image
	if (!config->skip_verify) {
		packfs_proccb_t vcbs = {
			.onerror = ifs_verify_onerror,
			.onbodyhash = ifs_verify_onbodyhash,
#ifdef CONFIG_PACKFS_IMAGEFS_VERBOSEINIT
			.onentrystart = ifs_verify_onentrystart,
#endif
			.onimgentryend = config->full_verify? ifs_verify_onimgentryend : NULL
		};
		bool verified = true;
		if (packfs_process_fromfile(imagefs_path, &vcbs, &verified) != ESP_OK || !verified) {
			ESP_LOGE(IMAGEFS_TAG, "Failed to verify pack file for imagefs: path=%s", imagefs_path);
			return ESP_FAIL;
		}
	}

	esp_vfs_t cb = {
		.flags = ESP_VFS_FLAG_DEFAULT,
		.open = ifs_open,
		.close = ifs_close,
		.read = ifs_read,
		.write = pfs_write,
		.lseek = ifs_lseek,
		.ioctl = ifs_ioctl,
		.fstat = ifs_fstat,
		.stat = ifs_stat,
		//.fcntl = ifs_fcntl,
		//.fsync = ifs_fsync,
		//.link = ifs_link,
		//.ulink = ifs_ulink,
		//.rename = ifs_rename,
		.opendir = ifs_opendir,
		.readdir = ifs_readdir,
		.readdir_r = ifs_readdir_r,
		.telldir = ifs_telldir,
		.seekdir = ifs_seekdir,
		.closedir = ifs_closedir,
		//.mkdir = ifs_mkdir,
		//.rmdir = ifs_rmdir,
		.access = ifs_access,
		//.truncate = ifs_truncate,
		//.utime = ifs_utime,
	};
	if ((err = esp_vfs_register(imagefs_mount, &cb, NULL)) != ESP_OK) {
		ESP_LOGE(IMAGEFS_TAG, "Unable to register imagefs vfs: err=%d", err);
		return err;
	}

	return ESP_OK;
}

/*esp_err_t imagefs_packhash(uint8_t outhash[32]) {
	// Sanity check
	if unlikely(imagefs_path[0] == '\0') {
		return ESP_ERR_INVALID_STATE;
	}

	// Create sha256 context
	mbedtls_sha256_context ctx;
	mbedtls_sha256_init(&ctx);
	if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {
		ESP_LOGE(IMAGEFS_TAG, "Failed to start hash context");
		mbedtls_sha256_free(&ctx);
		return ESP_FAIL;
	}

	// Open packfile
	FILE * fp = fopen(imagefs_path, "r");
	if (fp == NULL) {
		ESP_LOGE(IMAGEFS_TAG, "Could not open imagefs pack file");
		mbedtls_sha256_free(&ctx);
		return ESP_FAIL;
	}

	// Read in file to sha256 using outhash as buffer
	while (true) {
		size_t bytes = fread(outhash, 1, 32, fp);
		if (bytes == 0 && feof(fp)) {
			break;
		}

		ESP_LOGI(IMAGEFS_TAG, "Adding bytes to sha256: length=%zu", bytes);
		esp_log_buffer_hex(IMAGEFS_TAG, outhash, bytes);

		if (mbedtls_sha256_update_ret(&ctx, outhash, bytes) != 0) {
			ESP_LOGE(IMAGEFS_TAG, "Could not update imagefs hash context");
			mbedtls_sha256_free(&ctx);
			fclose(fp);
			return ESP_FAIL;
		}
	}
	fclose(fp);

	// Complete sha256
	if (mbedtls_sha256_finish_ret(&ctx, outhash) != 0) {
		ESP_LOGE(IMAGEFS_TAG, "Could not finish imagefs hash context");
		mbedtls_sha256_free(&ctx);
		return ESP_FAIL;
	}
	mbedtls_sha256_free(&ctx);

	return ESP_OK;
}*/

esp_err_t imagefs_cleanfs(imagefs_clean_t * cbs) {
	// Sanity check args
	if unlikely(cbs == NULL || cbs->shouldclean == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if unlikely(!ifs_checkinit()) {
		return ESP_ERR_INVALID_STATE;
	}

	struct dirent * entry;
	DIR * dir = opendir(iprefix_path);
	if (dir == NULL) {
		return ESP_FAIL;
	}

	size_t lenprefix = strlen(iprefix_path) + 1;
	while ((entry = readdir(dir)) != NULL) {
		if (strcmp(entry->d_name, &imagefs_path[lenprefix]) == 0) {
			// The imagefs file we've currently loaded
			continue;
		}

		if (cbs->shouldclean(entry->d_name)) {
			char path[PACKFS_MAX_FULLPATH];
			if (snprintf(path, PACKFS_MAX_FULLPATH, "%s/%s", iprefix_path, entry->d_name) >= PACKFS_MAX_FULLPATH) {
				// Skip, path too long
				ESP_LOGW(IMAGEFS_TAG, "Should delete file %s, but skipping because the path is too long", entry->d_name);
				continue;
			}

			ESP_LOGW(IMAGEFS_TAG, "Cleaning unused file: %s", path);
			remove(path);

		}
	}

	closedir(dir);
	return ESP_OK;
}
