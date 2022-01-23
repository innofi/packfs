#ifndef __PACKFS_H_
#define __PACKFS_H_

#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#define CONFIG_PACKFS_LZO_SUPPORT
#define CONFIG_PACKFS_PROCESS_SUPPORT
#define CONFIG_PACKFS_STREAM_SUPPORT

#define PACKFS_PATH_SEPERATOR	'#'
#define PACKFS_MAX_FULLPATH		(96)
#define PACKFS_MAX_METAKEY		(32)
#define PACKFS_MAX_METAVALUE	(64)
#define PACKFS_MAX_ENTRYPATH	(64)
#define PACKFS_MAX_NUMENTRIES	(30)

#define PACKFS_MAX_LZOBLOCK		(2048)
#define PACKFS_MIN_STREAMSIZE	(128) /* minimum size = max(sizeof(packfs_entry_t), sizeof(packfs_meta_t)) */
#define PACKFS_HASHSIZE			(32)


#define __packfs_packed __attribute__((packed))

typedef struct __packfs_packed {
	uint16_t magic;
	uint8_t version;
	uint16_t metasize;
	uint16_t indexsize;
	uint32_t regentrysize;
	uint32_t imgentrysize;
	uint8_t packhash[PACKFS_HASHSIZE];
	uint16_t headercrc;
} packfs_header_t;

typedef struct __packfs_packed {
	uint8_t flags;
	char key[PACKFS_MAX_METAKEY];
	char value[PACKFS_MAX_METAVALUE];
} packfs_meta_t;

#define PT_REG		(0x01)
#define PT_IMG		(0x02)
#define PF_LZO		(0x10)
typedef struct __packfs_packed {
	uint32_t offset;
	uint32_t length;
	uint8_t flags;
	char path[PACKFS_MAX_ENTRYPATH];
} packfs_entry_t;

#ifdef CONFIG_PACKFS_PROCESS_SUPPORT
typedef enum {
	PS_OK,
	PS_FAIL,
	PS_AGAIN,
	PS_EOF,
	PS_HASHNOMATCH,
	PS_USERBAIL
} packfs_status_t;

typedef enum {
	PS_HEADER		= 0,
	PS_META			= 1,
	PS_INDEX		= 2,
	PS_REGENTRY		= 3,
	PS_IMGENTRY		= 4
} packfs_proc_section_t;

typedef struct {
	void (*onerror)(void * ud, const char * file, unsigned int line, packfs_proc_section_t section, int err);
	void (*onheader)(void * ud, const packfs_header_t * header);
	void (*onmeta)(void * ud, const packfs_meta_t * meta);
	bool (*onbodyhash)(void * ud, uint8_t * reported_hash, uint8_t * calculated_hash, bool hash_matches);
	bool (*onentrystart)(void * ud, const packfs_entry_t * entry, uint32_t filesize);
	void (*onentrydata)(void * ud, const packfs_entry_t * entry, void * data, uint32_t length, uint32_t offset);
	bool (*onregentryend)(void * ud, const packfs_entry_t * entry);
	bool (*onimgentryend)(void * ud, const packfs_entry_t * entry, uint8_t * reported_hash, uint8_t * calculated_hash, bool hash_matches);
	bool (*oneof)(void * ud);
} packfs_proccb_t;

typedef void * packfs_process_t;

#ifdef CONFIG_PACKFS_STREAM_SUPPORT
typedef void * packfs_stream_t;
#endif
#endif

typedef struct {
	const char * base_path;
	const char * prefix_path;
	size_t max_files;
} packfs_conf_t;

#define PIOCTL_METASIZE			(1)
#define PIOCTL_METAFINDINDEX	(2)
#define PIOCTL_METAFINDNAME		(3)

#define PIOCTL_ENTRYSIZE		(4)
#define PIOCTL_ENTRYFINDINDEX	(5)
#define PIOCTL_ENTRYFINDPATH	(6)

#define PIOCTL_CURRENTENTRY		(7)
#define PIOCTL_CURRENTIMGHASH	(8)

esp_err_t packfs_vfs_register(packfs_conf_t * config);

#ifdef CONFIG_PACKFS_PROCESS_SUPPORT
esp_err_t packfs_process_fromfile(const char * filepath, packfs_proccb_t * cbs, void * userdata);
void packfs_process_free(packfs_process_t proc);
#ifdef CONFIG_PACKFS_STREAM_SUPPORT
packfs_status_t packfs_stream_process(packfs_stream_t stream);
ssize_t packfs_stream_load(packfs_stream_t stream, void * data, size_t length);
packfs_status_t packfs_stream_loadeof(packfs_stream_t stream);
packfs_status_t packfs_stream_flush(packfs_stream_t stream);
packfs_status_t packfs_stream_loadandprocess(packfs_stream_t stream, void * data, size_t length);
packfs_status_t packfs_stream_loadeofandflush(packfs_stream_t stream);

esp_err_t packfs_stream_tofile(FILE * fp, size_t bufsize, packfs_proccb_t * cbs, void * userdata, packfs_stream_t * out_stream);
esp_err_t packfs_stream_tofile_close(packfs_stream_t stream);
#endif
#endif

#endif
