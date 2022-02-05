#include <errno.h>
#include <unistd.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include "packfs-priv.h"
#include "imagefs-priv.h"


#ifndef CONFIG_IMAGEFS_SUPPORT
#error "This file should NOT be included if CONFIG_PACKFS_IMAGEFS_SUPPORT is not set."
#else

typedef struct {
	int eerrno;
	esp_err_t err;
	bool foundimg;
	char path[PACKFS_MAX_INDEXPATH];
	const esp_partition_t * partition;
	esp_ota_handle_t handle;
} ifs_dfu_t;

typedef struct {
	bool stripimg;
	bool reachedeof;
	char scratchpath[PACKFS_MAX_FULLPATH];
} ifss_dfu_t;

static void ifs_despartitions(const esp_partition_t * update) {
	esp_err_t err = ESP_OK;
	esp_app_desc_t desc;

	const esp_partition_t * boot = esp_ota_get_boot_partition();
	const esp_partition_t * running = esp_ota_get_running_partition();

	if (boot != running) {
		ESP_LOGW(IMAGEFS_DFU_TAG, "Configured OTA boot partition at address 0x%08x, but running from address 0x%08x", boot->address, running->address);
		ESP_LOGW(IMAGEFS_DFU_TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
	}
	ESP_LOGI(IMAGEFS_DFU_TAG, "Running partition %s (address 0x%08x) type %d subtype %d", running->label, running->address, running->type, running->subtype);
	if ((err = esp_ota_get_partition_description(running, &desc)) == ESP_OK) {
		ESP_LOGI(IMAGEFS_DFU_TAG, "Running app name %s version %s (compiled %s %s with idf %s)", desc.project_name, desc.version, desc.date, desc.time, desc.idf_ver);
	} else {
		ESP_LOGW(IMAGEFS_DFU_TAG, "Unable to query running app description");
	}
	ESP_LOGI(IMAGEFS_DFU_TAG, "Writing to partition %s (address 0x%08x) type %d subtype %d", update->label, update->address, update->type, update->subtype);
	if ((err = esp_ota_get_partition_description(update, &desc)) == ESP_OK) {
		ESP_LOGI(IMAGEFS_DFU_TAG, "Overwriting app name %s version %s (compiled %s %s with idf %s)", desc.project_name, desc.version, desc.date, desc.time, desc.idf_ver);
	} else {
		ESP_LOGI(IMAGEFS_DFU_TAG, "Overwriting app not valid (blank region or corrupted)");
	}
}

static esp_err_t ifs_descpartition(const esp_partition_t * update, esp_app_desc_t * desc) {
	esp_err_t err = ESP_OK;
	if ((err = esp_ota_get_partition_description(update, desc)) == ESP_OK) {
		ESP_LOGI(IMAGEFS_DFU_TAG, "Wrote app name %s version %s (compiled %s %s with idf %s)", desc->project_name, desc->version, desc->date, desc->time, desc->idf_ver);
	} else {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Unable to query description for newly-written app. Corrupted?");
	}
	return err;
}

static inline bool ifs_fileexists(const char * filepath) {
	struct stat st;
	return stat(filepath, &st) == 0;
}

static bool ifs_filemove(const char * topath, const char * frompath) {
	if (strcmp(topath, frompath) == 0) {
		return true;
	}

	if (ifs_fileexists(topath)) {
		ESP_LOGW(IMAGEFS_DFU_TAG, "File %s already exists, removing first.", topath);
		if (remove(topath) != 0) {
			ESP_LOGE(IMAGEFS_DFU_TAG, "Unable to remove file %s.", topath);
			return false;
		}
	}

	if (rename(frompath, topath) != 0) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed to rename %s to %s", frompath, topath);
		return false;
	}

	return true;
}

static esp_err_t ifs_fixfilename(esp_app_desc_t * app, const char * filepath) {
	char goodpath[PACKFS_MAX_FULLPATH];
	if (!ifs_imagepath(app, goodpath, sizeof(goodpath))) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed to set up imagefs naming convention");
		return ESP_ERR_INVALID_SIZE;
	}

	if (!ifs_filemove(goodpath, filepath)) {
		return ESP_FAIL;
	}

	return ESP_OK;
}

static void ifs_dfu_onerror(void * ud, const char * file, unsigned int line, packfs_proc_section_t section, int err) {
	ESP_LOGE(IMAGEFS_DFU_TAG, "Critical Error during DFU! (file=%s, line=%u, section=%d, errno=%d)", file, line, section, err);
	((ifs_dfu_t *)ud)->eerrno = err;
}

static bool ifs_dfu_onbodyhash(void * ud, uint8_t * reported_hash, uint8_t * calculated_hash, bool hash_matches) {
	if (!hash_matches) {
		ESP_LOGW(IMAGEFS_DFU_TAG, "Verification hash failure. Corrupt DFU file?");
		return false;
	}
	return true;
}

static bool ifs_dfu_onentrystart(void * ud, const packfs_entry_t * entry, uint32_t filesize) {
	ifs_dfu_t * dfu = (ifs_dfu_t *)ud;

	// Determine if we should start dfu
	bool start = !dfu->foundimg && (entry->flags & PFT_IMG) && strcmp(entry->path, dfu->path) == 0;
	if (!start) return false;

	// Mark image as found
	dfu->foundimg = true;

	// Start the OTA
	if ((dfu->err = esp_ota_begin(dfu->partition, filesize, &dfu->handle)) != ESP_OK) {
		return false;
	}

	return true;
}

static void ifs_dfu_onentrydata(void * ud, const packfs_entry_t * entry, void * data, uint32_t length, uint32_t offset) {
	ifs_dfu_t * dfu = (ifs_dfu_t *)ud;

	// Sanity check
	if unlikely(dfu->err != ESP_OK) {
		return;
	}

	dfu->err = esp_ota_write(dfu->handle, data, length);
}

static bool ifs_dfu_onimgentryend(void * ud, const packfs_entry_t * entry, uint8_t * reported_hash, uint8_t * calculated_hash, bool hash_matches) {
	ifs_dfu_t * dfu = (ifs_dfu_t *)ud;

	// Sanity check
	if unlikely(dfu->err != ESP_OK) {
		return false;
	}

	// End the OTA
	esp_err_t err = esp_ota_end(dfu->handle);
	if (dfu->err == ESP_OK && err != ESP_OK) {
		dfu->err = err;
	}
	dfu->handle = 0;

	if (!hash_matches) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Verification hash failure. Corrupt image in DFU file?");
		if (dfu->err == ESP_OK) dfu->err = ESP_ERR_IMAGE_INVALID;
	}

	return true;
}

esp_err_t imagefs_file_dfu(const char * file_path, const char * firmware_image_subpath, bool ensure_mountable) {
	// Sanity check
	if unlikely(file_path == NULL || firmware_image_subpath == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (strlen(firmware_image_subpath) > (PACKFS_MAX_INDEXPATH - 1)) {
		return ESP_ERR_INVALID_SIZE;
	}

	// Check for subsystem integrity
	if (ensure_mountable && !ifs_checkinit(false)) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Cannot ensure_mountable without a configured imagefs subsystem");
		ESP_LOGE(IMAGEFS_DFU_TAG, "(Must call either imagefs_vfs_register or imagefs_filename_register first)");
		return ESP_FAIL;
	}

	ESP_LOGI(IMAGEFS_DFU_TAG, "Performing DFU with file %s", file_path);

	// Ensure DFU file exists
	if (!ifs_fileexists(file_path)) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "File %s does not exist.", file_path);
		return ESP_FAIL;
	}

	const esp_partition_t * update = esp_ota_get_next_update_partition(NULL);
	if (update == NULL) {
		ESP_LOGW(IMAGEFS_DFU_TAG, "Unable to perform DFU, no ota partition found.");
		return ESP_FAIL;
	}
	ifs_despartitions(update);

	// Perform DFU
	{
		ifs_dfu_t dfu = {
			.eerrno = 0,
			.err = ESP_OK,
			.foundimg = false,
			.handle = 0,
			.partition = update
		};
		strcpy(dfu.path, firmware_image_subpath);
		packfs_proccb_t cbs = {
			.onerror = ifs_dfu_onerror,
			.onbodyhash = ifs_dfu_onbodyhash,
			.onentrystart = ifs_dfu_onentrystart,
			.onentrydata = ifs_dfu_onentrydata,
			.onimgentryend = ifs_dfu_onimgentryend,
		};
		esp_err_t result = packfs_process_fromfile(file_path, &cbs, &dfu);
		if (result != ESP_OK || dfu.eerrno != 0 || dfu.err != ESP_OK) {
			ESP_LOGE(IMAGEFS_DFU_TAG, "Failed DFU update. Result error %d, errno %d, nested error %d", result, dfu.eerrno, dfu.err);
			return ESP_FAIL;
		}

		if (!dfu.foundimg) {
			ESP_LOGE(IMAGEFS_DFU_TAG, "Failed DFU update. Firmware subpath %s not found", firmware_image_subpath);
			return ESP_FAIL;
		}
	}

	esp_err_t err = ESP_OK;

	esp_app_desc_t app;
	if ((err = ifs_descpartition(update, &app)) != ESP_OK) {
		return err;
	}

	// Rename file if needed
	if (ensure_mountable && (err = ifs_fixfilename(&app, file_path)) != ESP_OK) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed DFU update. Could not ensure mountable.");
		return err;
	}

	if ((err = esp_ota_set_boot_partition(update)) != ESP_OK) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed to make update partition bootable.");
		return err;
	}

	ESP_LOGI(IMAGEFS_DFU_TAG, "Firmware DFU complete. OK to reboot");
	return ESP_OK;
}

static packfs_status_t ifss_dfu_write(pfs_proc_t * proc, void * data, size_t length) {
	ifs_dfu_t * dfu = pfss_extra(pfsp_extra(proc));
	ifss_dfu_t * sdfu = (void *)&dfu[1];

	if (sdfu->stripimg && proc->section == PS_IMGENTRY) {
		// Don't write, we want to strip this section
		return PS_OK;
	}

	packfs_status_t status = fwrite(data, length, 1, proc->ctx.backing) == 1? PS_OK : PS_FAIL;
	if (status != PS_OK) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Firmware DFU write error: errno=%d", errno);
	}

	return status;
}

static bool ifss_dfu_oneof(void * ud) {
	ifs_dfu_t * dfu = ud;
	ifss_dfu_t * sdfu = (void *)&dfu[1];

	sdfu->reachedeof = true;
	return true;
}

esp_err_t imagefs_stream_dfu(const char * firmware_image_subpath, bool strip_image_section, packfs_stream_t * out_stream) {
	labels(procerr); // @suppress("Type cannot be resolved")

	// Sanity check
	if unlikely(firmware_image_subpath == NULL || out_stream == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if (strlen(firmware_image_subpath) > (PACKFS_MAX_INDEXPATH - 1)) {
		return ESP_ERR_INVALID_SIZE;
	}

	// Check for subsystem integrity
	if (!ifs_checkinit(false)) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Cannot continue without a configured imagefs subsystem");
		ESP_LOGE(IMAGEFS_DFU_TAG, "(Must call either imagefs_vfs_register or imagefs_filename_register first)");
		return ESP_FAIL;
	}

	const esp_partition_t * update = esp_ota_get_next_update_partition(NULL);
	if (update == NULL) {
		ESP_LOGW(IMAGEFS_DFU_TAG, "Unable to perform DFU, no ota partition found.");
		return ESP_FAIL;
	}
	ifs_despartitions(update);

	// Allocate and proc structure
	pfsp_io_t ios = {
		.read = pfss_read,
		.write = ifss_dfu_write
	};
	packfs_proccb_t cbs = {
		.onerror = ifs_dfu_onerror,
		.onbodyhash = ifs_dfu_onbodyhash,
		.onentrystart = ifs_dfu_onentrystart,
		.onentrydata = ifs_dfu_onentrydata,
		.onimgentryend = ifs_dfu_onimgentryend,
		.oneof = ifss_dfu_oneof
	};
	pfs_proc_t * proc = pfss_create(IMAGEFS_DFU_STREAM_BUFSIZE, &ios, &cbs, NULL, sizeof(ifs_dfu_t) + sizeof(ifss_dfu_t));
	if (proc == NULL) {
		return ESP_ERR_NO_MEM;
	}

	// Initialize internal variables
	ifs_dfu_t * dfu = pfss_extra(pfsp_extra(proc));
	ifss_dfu_t * sdfu = (void *)&dfu[1];
	proc->userdata = dfu;
	*dfu = (ifs_dfu_t){
		.eerrno = 0,
		.err = ESP_OK,
		.foundimg = false,
		.handle = 0,
		.partition = update
	};
	strcpy(dfu->path, firmware_image_subpath);
	*sdfu = (ifss_dfu_t){
		.stripimg = strip_image_section,
		.reachedeof = false
	};

	// Return err code
	esp_err_t err = ESP_OK;

	// Create scratch path
	if (!ifs_scratchpath(sdfu->scratchpath, sizeof(sdfu->scratchpath))) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed to set up imagefs naming convention");
		errgoto(ESP_ERR_INVALID_SIZE, procerr);
	}

	// Check for existence and delete
	if (ifs_fileexists(sdfu->scratchpath) && remove(sdfu->scratchpath) != 0) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed to initialize scratch file");
		errgoto(ESP_FAIL, procerr);
	}

	// Open backing file
	if ((proc->ctx.backing = fopen(sdfu->scratchpath, "w")) == NULL) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed to open backing file");
		errgoto(ESP_FAIL, procerr);
	}

	ESP_LOGI(IMAGEFS_DFU_TAG, "DFU Stream started");
	*out_stream = (packfs_stream_t)proc;
	return ESP_OK;

procerr:
	pfsp_free(proc);
	return err;
}

esp_err_t imagefs_stream_dfu_complete(packfs_stream_t stream) {
	labels(procerr); // @suppress("Type cannot be resolved")
	pfs_proc_t * proc = (pfs_proc_t *)stream;

	// Sanity check
	if unlikely(proc == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	// Internal state
	ifs_dfu_t * dfu = pfss_extra(pfsp_extra(proc));
	ifss_dfu_t * sdfu = (void *)&dfu[1];

	esp_err_t err = ESP_OK;

	// Push EOF if necessary
	if (proc->state != PS_CLOSED && packfs_stream_loadeofandflush(stream) != PS_EOF) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed DFU update. Could not flush stream.");
		err = ESP_FAIL;
	}

	// Close the backing file, make sure we always do this
	if (proc->ctx.backing != NULL) {
		fflush(proc->ctx.backing);
		fclose(proc->ctx.backing);
		proc->ctx.backing = NULL;
	}

	if (err == ESP_FAIL) {
		errgoto(ESP_FAIL, procerr);
	}

	// Check for err codes
	if (dfu->eerrno != 0 || dfu->err != ESP_OK) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed DFU update. Result errno %d, nested error %d", dfu->eerrno, dfu->err);
		errgoto(ESP_FAIL, procerr);
	}

	// Make sure we've completed processing
	if (!sdfu->reachedeof) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed DFU update. Stream not completely processed");
		errgoto(ESP_FAIL, procerr);
	}

	// Make sure we've found the firmware file
	if (!dfu->foundimg) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed DFU update. Firmware subpath %s not processed", dfu->path);
		errgoto(ESP_FAIL, procerr);
	}

	esp_app_desc_t app;
	if ((err = ifs_descpartition(dfu->partition, &app)) != ESP_OK) {
		goto procerr;
	}

	// Rename file
	if ((err = ifs_fixfilename(&app, sdfu->scratchpath)) != ESP_OK) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed DFU update. Could not ensure mountable.");
		goto procerr;
	}

	if ((err = esp_ota_set_boot_partition(dfu->partition)) != ESP_OK) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Failed to make update partition bootable.");
		goto procerr;
	}

	ESP_LOGI(IMAGEFS_DFU_TAG, "Firmware DFU complete. OK to reboot");

procerr:
	pfsp_close(proc);
	pfsp_free(proc);
	return err;
}

esp_err_t imagefs_stream_dfu_cancel(packfs_stream_t stream) {
	pfs_proc_t * proc = (pfs_proc_t *)stream;

	// Sanity check
	if unlikely(proc == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	// Internal state
	ifs_dfu_t * dfu = pfss_extra(pfsp_extra(proc));
	ifss_dfu_t * sdfu = (void *)&dfu[1];

	// Close the OTA handle
	if (dfu->handle != 0) {
		esp_ota_end(dfu->handle);
		dfu->handle = 0;
	}

	// Close the backing file
	if (proc->ctx.backing != NULL) {
		fclose(proc->ctx.backing);
		proc->ctx.backing = NULL;
	}

	// Try to delete backing file
	if (remove(sdfu->scratchpath) != 0) {
		ESP_LOGE(IMAGEFS_DFU_TAG, "Unable to remove DFU scratch file %s.", sdfu->scratchpath);
	}

	ESP_LOGI(IMAGEFS_DFU_TAG, "Firmware DFU canceled.");

	pfsp_close(proc);
	pfsp_free(proc);
	return ESP_OK;
}

#endif
