#ifndef __IMAGEFS_PRIV_H_
#define __IMAGEFS_PRIV_H_

#include <esp_app_format.h>

#include <imagefs.h>


#define IMAGEFS_TAG				"IMAGEFS"
#define IMAGEFS_DFU_TAG			"IMAGEFS_DFU"


typedef struct {
	pfs_ctx_t pctx;
	enum {
		IM_OPENENTRY,
		IM_READMETA,
	} mode;
	uint32_t offset;
} ifs_ctx_t;


int ifs_newctx(void);
ifs_ctx_t * ifs_getctx(int fd);

bool ifs_checkinit();
bool ifs_imagepath(const esp_app_desc_t * app, char * path, size_t pathlen);
bool ifs_scratchpath(char * path, size_t pathlen);

// File ops
int ifs_open(const char * path, int flags, int mode);
int ifs_close(int fd);
ssize_t ifs_read(int fd, void * buffer, size_t length);
off_t ifs_lseek(int fd, off_t offset, int mode);
int ifs_ioctl(int fd, int cmd, va_list args);

int ifs_fstat(int fd, struct stat * st);
int ifs_stat(const char * path, struct stat * st);
int ifs_access(const char * path, int amode);

DIR * ifs_opendir(const char * path);
int ifs_closedir(DIR * pdir);
int ifs_readdir_r(DIR * pdir, struct dirent * entry, struct dirent ** out);
struct dirent * ifs_readdir(DIR * pdir);
long ifs_telldir(DIR * pdir);
void ifs_seekdir(DIR * pdir, long offset);


#endif
