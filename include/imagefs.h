#ifndef __IMAGEFS_H_
#define __IMAGEFS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

#include <packfs.h>

//#define CONFIG_PACKFS_IMAGEFS_SUPPORT
#define CONFIG_PACKFS_IMAGEFS_VERBOSEINIT

#define IMAGEFS_PATH_META				"/meta/"
#define IMAGEFS_DFU_STREAM_BUFSIZE		(128) /* minimum size = PACKFS_MIN_STREAMSIZE */

typedef struct {
	bool (*namegen)(char * path, size_t pathlen, const char * projname, const char * projversion);
	bool (*namecheck)(const char * path);
	bool (*scratchfile)(char * path, size_t pathlen);
} imagefs_filename_t;

typedef struct {
	bool (*shouldclean)(const char * path);
} imagefs_clean_t;

typedef struct {
	const char * base_path;
	const char * prefix_path;
	size_t max_files;
	bool skip_verify;
	bool full_verify;
	imagefs_filename_t filename;
} imagefs_conf_t;

esp_err_t imagefs_vfs_register(imagefs_conf_t * config);
esp_err_t imagefs_filename_register(const char * prefix_path, imagefs_filename_t * filename_funcs);
//esp_err_t imagefs_packhash(uint8_t outhash[32]);

esp_err_t imagefs_file_dfu(const char * file_path, const char * firmware_image_subpath, bool ensure_mountable);

esp_err_t imagefs_stream_dfu(const char * firmware_image_subpath, bool strip_image_section, packfs_stream_t * out_stream);
esp_err_t imagefs_stream_dfu_complete(packfs_stream_t stream);
esp_err_t imagefs_stream_dfu_cancel(packfs_stream_t stream);

esp_err_t imagefs_cleanfs(imagefs_clean_t * cbs);

#endif
