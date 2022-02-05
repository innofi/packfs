#ifndef __PACKFS_PRIV_H_
#define __PACKFS_PRIV_H_

#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#include <mbedtls/sha256.h>
#include <rom/crc.h>

#include <packfs.h>

#define PACKFS_VERSION			(1)
#define PACKFS_TAG				"PACKFS"

#define PACKFS_MAGIC			(0x12fc)
#define PACKFS_PROC_BUFSIZE		(128)		/* Minimum size 32 */


#ifdef unlikely
#undef unlikely
#endif

#define unlikely(x)     	(__builtin_expect(!!(x), 0))
#define labels(...)			__label__ __VA_ARGS__
#define min(a, b)			({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a < _b ? _a : _b; })
#define errnogoto(e, l)		({ errno = e; goto l; })
#define errgoto(e, l)		({ err = e; goto l; })

#define pfs_error(ctx)		((ctx)->errored)


#ifdef CONFIG_PACKFS_LZO_SUPPORT
typedef struct __packfs_packed {
	uint32_t uncompressed_length;
	uint16_t blocksize;
} pfs_lzoheader_t;

typedef struct {
	uint16_t compressed_length;
	uint8_t * compressed;
	uint16_t uncompressed_offset;
	uint16_t uncompressed_length;
	uint8_t * uncompressed;
} pfs_lzoblock_t;
#endif

typedef struct {
	bool inuse;
	bool errored;
	FILE * backing;
	uint32_t offset;
	union {
		packfs_meta_t meta;
		packfs_entry_t entry;
	};
#ifdef CONFIG_PACKFS_LZO_SUPPORT
	struct {
		uint16_t numblocks;
		pfs_lzoheader_t header;
		pfs_lzoblock_t block;
	} lzo;
#endif
} pfs_ctx_t;

typedef struct {
	DIR dir;
	struct dirent ent;
	uint32_t index_start;
	uint32_t index_length;
	uint32_t file_length;
	int fd;
} pfs_dirent_t;

#ifdef CONFIG_PACKFS_PROCESS_SUPPORT
struct pfs_proc_t;

typedef enum {
	PS_READHEADER,
	PS_READMETA,
	PS_READINDEX,
	PS_READENTRY,
	PS_SKIPENTRY,
	PS_READIMGHASH,
	PS_READREGCHUNK,
	PS_READLZOHEADER,
	PS_READLZOSIZE,
	PS_READLZOCHUNK,
	PS_CLOSED,
} pfsp_state_t;

typedef struct {
	packfs_status_t (*read)(struct pfs_proc_t * proc, void * data, size_t minlength, size_t maxlength, size_t * outlength);
	packfs_status_t (*write)(struct pfs_proc_t * proc, void * data, size_t length);
	//void (*close)(struct pfs_proc_t * proc);
} pfsp_io_t;

typedef enum {
	PP_FILE,
	PP_STREAM
} pfsp_type_t;

typedef struct pfs_proc_t {
	bool errored;
	pfsp_type_t type;
	packfs_proc_section_t section;
	pfsp_state_t state;
	pfs_ctx_t ctx;
	packfs_header_t header;
	packfs_entry_t * entries;
	size_t onentry;
	packfs_proccb_t cbs;
	pfsp_io_t ios;
	mbedtls_sha256_context * shactx;
	void * userdata;
	uint8_t extra[0];
} pfs_proc_t;

#ifdef CONFIG_PACKFS_STREAM_SUPPORT
typedef struct {
	size_t size;
	size_t offset;
	size_t length;
	bool eof;
	uint8_t buffer[0];
} pfs_stream_t;
#endif
#endif

// Inner functions
int pfs_newctx(void);
pfs_ctx_t * pfs_getctx(int fd);
bool pfs_prepentry(pfs_ctx_t * ctx);
const char * pfs_parsepath(const char * fullpath, char * root, size_t rootlen);
FILE * pfs_openbacking(const char * backingpath, uint32_t * length);

// LZO inner functions
#ifdef CONFIG_PACKFS_LZO_SUPPORT
bool pfs_lzomalloc(pfs_ctx_t * ctx);
void pfs_lzofree(pfs_ctx_t * ctx);
bool pfs_preplzo(pfs_ctx_t * ctx);
bool pfs_readlzoheader(pfs_ctx_t * ctx);
bool pfs_decompresslzoblock(pfs_ctx_t * ctx);
ssize_t pfs_readlzo(pfs_ctx_t * ctx, void * buffer, size_t length);
off_t pfs_seekfilelzo(pfs_ctx_t * ctx, off_t offset, int mode);
bool pfs_initlzo(void);
#endif

// Seek ops
bool pfs_seekabs(pfs_ctx_t * ctx, uint32_t offset);
bool pfs_seekfwd(pfs_ctx_t * ctx, uint32_t length);
bool pfs_seekentry(pfs_ctx_t * ctx, packfs_entry_t * entry);

// Find ops
bool pfs_findmeta(pfs_ctx_t * ctx, uint32_t metasize, const char * key, unsigned int * out_index);
bool pfs_findentry(pfs_ctx_t * ctx, uint32_t indexsize, const char * path, packfs_entry_t * out_entry);

// Read ops
bool pfs_readchunk(pfs_ctx_t * ctx, void * buffer, size_t length);
bool pfs_readmeta(pfs_ctx_t * ctx, packfs_meta_t * meta, char * desc, uint8_t * value);
bool pfs_readindex(pfs_ctx_t * ctx, packfs_entry_t * entry);
//bool pfs_readimghash(pfs_ctx_t * ctx, uint8_t * hash);

// Write ops
ssize_t pfs_write(int fd, const void * data, size_t size);

// Check ops
bool pfs_checkinit(void);
bool pfs_checkheader(packfs_header_t * header);
#ifdef CONFIG_PACKFS_LZO_SUPPORT
bool pfs_checklzoheader(pfs_ctx_t * ctx);
bool pfs_checklzoblock(pfs_ctx_t * ctx);
#endif

// Generic x-functions
bool xfs_open(pfs_ctx_t * ctx, const char * spiffspath, const char * subpath, uint32_t * out_length, packfs_header_t * out_header);
void xfs_close(pfs_ctx_t * ctx);
ssize_t xfs_read(pfs_ctx_t * ctx, void * buffer, size_t length);
off_t xfs_lseek(pfs_ctx_t * ctx, off_t offset, int mode);
int xfs_ioctl(pfs_ctx_t * ctx, int cmd, va_list args);
int xfs_fstat(pfs_ctx_t * ctx, struct stat * st);

pfs_dirent_t * xfs_opendir(pfs_ctx_t * ctx, const char * path, int fd);
int xfs_readdir_r(pfs_ctx_t * ctx, pfs_dirent_t * dir, struct dirent * entry, struct dirent ** out);
long xfs_telldir(pfs_ctx_t * ctx, pfs_dirent_t * dir);

// File ops
int pfs_open(const char * path, int flags, int mode);
int pfs_close(int fd);
ssize_t pfs_read(int fd, void * buffer, size_t length);
off_t pfs_lseek(int fd, off_t offset, int mode);
int pfs_ioctl(int fd, int cmd, va_list args);

// Stat ops
int pfs_fstat(int fd, struct stat * st);
int pfs_stat(const char * path, struct stat * st);
int pfs_access(const char * path, int amode);

// Directory ops
DIR * pfs_opendir(const char * path);
int pfs_closedir(DIR * pdir);
struct dirent * pfs_readdir(DIR * pdir);
int pfs_readdir_r(DIR * pdir, struct dirent * entry, struct dirent ** out);
void pfs_seekdir(DIR * pdir, long offset);
long pfs_telldir(DIR * pdir);

#ifdef CONFIG_PACKFS_PROCESS_SUPPORT
pfs_proc_t * pfsp_malloc(void * userdata, pfsp_type_t type, pfsp_io_t * ios, packfs_proccb_t * cbs, bool hashmem, size_t extrasize);
void * pfsp_extra(pfs_proc_t * proc);
void pfsp_free(pfs_proc_t * proc);
packfs_status_t pfsp_fromfile_read(pfs_proc_t * proc, void * data, size_t minlength, size_t maxlength, size_t * outlength);
packfs_status_t pfsp_tofile_write(pfs_proc_t * proc, void * data, size_t length);
packfs_status_t pfsp_process(pfs_proc_t * proc);
void pfsp_close(pfs_proc_t * proc);
#ifdef CONFIG_PACKFS_STREAM_SUPPORT
pfs_proc_t * pfss_create(size_t size, pfsp_io_t * ios, packfs_proccb_t * cbs, void * userdata, size_t extrasize);
packfs_status_t pfss_read(pfs_proc_t * proc, void * data, size_t minlength, size_t maxlength, size_t * outlength);
void * pfss_extra(pfs_stream_t * stream);
#endif
#endif

#endif
