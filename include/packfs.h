#ifndef __PACKFS_H_
#define __PACKFS_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <esp_err.h>

//#define CONFIG_PACKFS_PROCESS_SUPPORT
//#define CONFIG_PACKFS_STREAM_SUPPORT

#define PACKFS_PATH_SEPERATOR	'#'
#define PACKFS_MAX_FULLPATH		(96)
#define PACKFS_MAX_METAKEY		(64)
//#define PACKFS_MAX_METAVALUE	(64)
#define PACKFS_MAX_INDEXPATH	(128)
//#define PACKFS_MAX_NUMENTRIES	(30)

#define PACKFS_MAX_LZOBLOCK		(2048)
#define PACKFS_MIN_STREAMSIZE	(128) /* minimum size = max(sizeof(packfs_entry_t), sizeof(packfs_meta_t)) */
// TODO - figure out min streamsize with updated meta_t
//#define PACKFS_HASHSIZE			(32)

typedef uint8_t packfs_sha256_t[32];
typedef packfs_sha256_t packfs_hmac_t;


#define __packfs_packed __attribute__((packed))

typedef struct __packfs_packed {
	uint16_t magic;
	uint8_t version;
	uint8_t _reserved;
	uint32_t metasize;
	uint32_t indexsize;
	packfs_sha256_t metahash;
	packfs_sha256_t indexhash;
	uint32_t headercrc;
	packfs_hmac_t securehmac;
} packfs_header_t;

#define PF_SECURED  (0x1000)
#define PF_CHANGED  (0x0001)
#define PF_NVM      (0x0010)
#define PF_NVM_ONLYIFCHANGED (0x0020)

typedef enum __packfs_packed {
    PT_UNK		= (0x00),
    PT_BOOL     = (0x01),
	PT_UINT8    = (0x10),
	PT_INT8		= (0x11),
	PT_UINT16	= (0x20),
	PT_INT16	= (0x21),
	PT_UINT32   = (0x30),
	PT_INT32    = (0x31),
	PT_UINT64   = (0x40),
	PT_INT64    = (0x41),
	PT_DOUBLE   = (0x50),
	PT_STRING   = (0x60),
	PT_BLOB     = (0x70),
	PT_FILE		= (0x71)
} packfs_metatype_t;
typedef struct __packfs_packed {
	uint16_t flags;
	packfs_metatype_t type;
	uint16_t descsize;
	uint32_t valuesize;
	char key[PACKFS_MAX_METAKEY];
} packfs_meta_t;

#define PFT_REG     (0x01)
#define PFT_IMG     (0x02)
#define PF_LZO      (0x10)
typedef struct __packfs_packed {
	uint8_t flags;
	uint32_t offset;
	uint32_t length;
	packfs_sha256_t entryhash;
	char path[PACKFS_MAX_INDEXPATH];
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
	void (*onmeta)(void * ud, const packfs_meta_t * meta, const char * description, const uint8_t * value);
	bool (*onindex)(void * ud, const packfs_entry_t * entry);
	bool (*onpackhash)(void * ud, const packfs_header_t * header, packfs_sha256_t calcd_headerhash, packfs_sha256_t calcd_metahash, packfs_sha256_t calcd_indexhash);
	bool (*onpackhmac)(void * ud, const packfs_header_t * header, packfs_hmac_t calcd_securehmac);
	bool (*onentrystart)(void * ud, const packfs_entry_t * entry);
	bool (*onentryend)(void * ud, const packfs_entry_t * entry, packfs_sha256_t calcd_entryhash);
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
} packfs_conf_t;

#define PIOCTL_METACOUNT		(1)
#define PIOCTL_METAREAD			(2)
#define PIOCTL_METAFIND			(3)

#define PIOCTL_INDEXCOUNT		(4)
#define PIOCTL_INDEXREAD		(5)
#define PIOCTL_INDEXFIND		(6)

#define PIOCTL_ENTRYCURRENT		(7)
#define PIOCTL_ENTRYHASH		(8)

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
