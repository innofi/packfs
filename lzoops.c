#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <esp_err.h>
#include <esp_log.h>

#include "minilzo.h"
#include "packfs-priv.h"

#ifndef CONFIG_PACKFS_LZO_SUPPORT
#error "This file should NOT be included if CONFIG_PACKFS_LZO_SUPPORT is not set."
#else

bool pfs_lzomalloc(pfs_ctx_t * ctx) {
	if (ctx->lzo.block.compressed == NULL) {
		ctx->lzo.block.compressed = malloc(ctx->lzo.header.blocksize);
	} else {
		ctx->lzo.block.compressed = realloc(ctx->lzo.block.compressed, ctx->lzo.header.blocksize);
	}

	if (ctx->lzo.block.uncompressed == NULL) {
		ctx->lzo.block.uncompressed = malloc(ctx->lzo.header.blocksize);
	} else {
		ctx->lzo.block.uncompressed = realloc(ctx->lzo.block.uncompressed, ctx->lzo.header.blocksize);
	}

	ctx->lzo.block.compressed_length = 0;
	ctx->lzo.block.uncompressed_offset = ctx->lzo.block.uncompressed_length = 0;
	if unlikely(ctx->lzo.block.compressed == NULL || ctx->lzo.block.uncompressed == NULL) {
		pfs_lzofree(ctx);
		return false;
	}

	return true;
}

void pfs_lzofree(pfs_ctx_t * ctx) {
	if (ctx->lzo.block.compressed != NULL) {
		free(ctx->lzo.block.compressed);
		ctx->lzo.block.compressed = NULL;
	}
	if (ctx->lzo.block.uncompressed != NULL) {
		free(ctx->lzo.block.uncompressed);
		ctx->lzo.block.uncompressed = NULL;
	}
}

static inline size_t pfs_lzoposition(pfs_ctx_t * ctx) {
	return (ctx->lzo.numblocks == 0)? 0 : (size_t)(ctx->lzo.numblocks - 1) * (size_t)ctx->lzo.header.blocksize + (size_t)ctx->lzo.block.uncompressed_offset;
}

bool pfs_preplzo(pfs_ctx_t * ctx) {
	// Reset internal structures
	ctx->lzo.numblocks = 0;
	ctx->lzo.block.compressed_length = 0;
	ctx->lzo.block.uncompressed_offset = 0;
	ctx->lzo.block.uncompressed_length = 0;

	return true;
}

bool pfs_checklzoheader(pfs_ctx_t * ctx) {
	// Check for sane blocksize
	if (ctx->lzo.header.blocksize > PACKFS_MAX_LZOBLOCK) {
		pfs_error(ctx) = true;
		return false;
	}

	return true;
}

bool pfs_readlzoheader(pfs_ctx_t * ctx) {
	// Read lzo header
	if (!pfs_readchunk(ctx, &ctx->lzo.header, sizeof(pfs_lzoheader_t))) {
		return false;
	}

	return pfs_checklzoheader(ctx);
}

bool pfs_decompresslzoblock(pfs_ctx_t * ctx) {
	// Determine sizes and update internal pointers
	uint16_t uncompressed_len = min((uint32_t)ctx->lzo.header.blocksize, (uint32_t)ctx->lzo.header.uncompressed_length - (uint32_t)ctx->lzo.numblocks * (uint32_t)ctx->lzo.header.blocksize);
	ctx->lzo.numblocks += 1;
	ctx->lzo.block.uncompressed_offset = 0;
	ctx->lzo.block.uncompressed_length = uncompressed_len;

	// Handle incompressible block
	if (uncompressed_len == ctx->lzo.block.compressed_length) {
		memcpy(ctx->lzo.block.uncompressed, ctx->lzo.block.compressed, uncompressed_len);
		return true;
	}

	// Decompress
	lzo_uint outlen = uncompressed_len;
	int ret = lzo1x_decompress_safe(ctx->lzo.block.compressed, ctx->lzo.block.compressed_length, ctx->lzo.block.uncompressed, &outlen, NULL);
	if (ret != LZO_E_OK) {
		//ESP_LOGE(PACKFS_TAG, "Decompress failure: code=%d, block=%d", ret, ctx->lzo.numblocks - 1);
		return false;
	}

	// Double check lengths
	if (uncompressed_len != outlen) {
		return false;
	}

	return true;
}

bool pfs_checklzoblock(pfs_ctx_t * ctx) {
	// Verify block size is sane
	if (ctx->lzo.block.compressed_length > ctx->lzo.header.blocksize) {
		return false;
	}

	return true;
}

static bool pfs_readlzoblock(pfs_ctx_t * ctx) {
	// Verify work memory is allocated
	if ((ctx->lzo.block.compressed == NULL || ctx->lzo.block.uncompressed == NULL) && !pfs_lzomalloc(ctx)) {
		return false;
	}

	// Get compressed block size
	if (!pfs_readchunk(ctx, &ctx->lzo.block.compressed_length, sizeof(ctx->lzo.block.compressed_length))) {
		return false;
	}

	// Check for valid block
	if (!pfs_checklzoblock(ctx)) {
		return false;
	}

	// Read compressed block
	if (!pfs_readchunk(ctx, ctx->lzo.block.compressed, ctx->lzo.block.compressed_length)) {
		return false;
	}

	return pfs_decompresslzoblock(ctx);
}

ssize_t pfs_readlzo(pfs_ctx_t * ctx, void * buffer, size_t length) {
	labels(readerr); // @suppress("Type cannot be resolved")

	// Read blocks at a time
	ssize_t totalread = 0;
	while (length > 0) {
		// Load another block if necessary
		if (ctx->lzo.block.uncompressed_offset == ctx->lzo.block.uncompressed_length) {
			if (pfs_lzoposition(ctx) == ctx->lzo.header.uncompressed_length) {
				// No more blocks to load (eof)
				break;
			}

			if (!pfs_readlzoblock(ctx)) {
				errnogoto(EIO, readerr);
			}
		}

		// Copy from uncompressed block to user
		uint16_t bytes = min(length, (size_t)ctx->lzo.block.uncompressed_length - (size_t)ctx->lzo.block.uncompressed_offset);
		if (buffer != NULL) {
			memcpy(&((uint8_t *)buffer)[totalread], &ctx->lzo.block.uncompressed[ctx->lzo.block.uncompressed_offset], bytes);
		}
		ctx->lzo.block.uncompressed_offset += bytes;
		totalread += bytes;
		length -= bytes;
	}

	// Return amount of bytes read
	return totalread;

readerr:
	pfs_error(ctx) = true;
	return -1;
}

static bool pfs_skiplzoblock(pfs_ctx_t * ctx) {
	// Get compressed block size
	if (!pfs_readchunk(ctx, &ctx->lzo.block.compressed_length, sizeof(uint16_t))) {
		return false;
	}

	// Verify block size is sane
	if (ctx->lzo.block.compressed_length > ctx->lzo.header.blocksize) {
		return false;
	}

	// Skip past compressed block
	if (!pfs_seekfwd(ctx, ctx->lzo.block.compressed_length)) {
		return false;
	}

	// Determine sizes and update internal pointers
	uint16_t uncompressed_len = min((uint32_t)ctx->lzo.header.blocksize, (uint32_t)ctx->lzo.header.uncompressed_length - (uint32_t)ctx->lzo.numblocks * (uint32_t)ctx->lzo.header.blocksize);
	ctx->lzo.numblocks += 1;
	ctx->lzo.block.uncompressed_offset = uncompressed_len;
	ctx->lzo.block.uncompressed_length = uncompressed_len;

	return true;
}

off_t pfs_seekfilelzo(pfs_ctx_t * ctx, off_t offset, int mode) {
	labels(seekerr); // @suppress("Type cannot be resolved")

	// Make offset referenced from start of entry
	if (mode == SEEK_CUR) {
		offset += pfs_lzoposition(ctx);
	} else if (mode == SEEK_END) {
		offset += ctx->lzo.header.uncompressed_length;
	}

	// Make sure we're within file size
	if (offset < 0 || offset > ctx->lzo.header.uncompressed_length) {
		errno = EOVERFLOW;
		return -1;
	}

	uint32_t position = pfs_lzoposition(ctx);
	if (offset == position) {
		// Nothing to do, already at seek point
		return offset;

	} else if (offset >= (position - ctx->lzo.block.uncompressed_offset) && offset < (position - ctx->lzo.block.uncompressed_offset + ctx->lzo.block.uncompressed_length)) {
		// Offset is within current block, rewind to beginning of block
		position -= ctx->lzo.block.uncompressed_offset;
		ctx->lzo.block.uncompressed_offset = 0;

	} else if (offset < position) {
		// Offset is behind us, rewind to beginning of entry and reset fields
		if (!pfs_seekentry(ctx, &ctx->entry) || !pfs_prepentry(ctx)) {
			errnogoto(EIO, seekerr);
		}

		position = 0;
	}

	// Now seek forward in compressed file
	while (position < offset) {
		uint16_t bytesleft = (uint16_t)((uint32_t)offset - position);

		if (ctx->lzo.block.uncompressed_offset < ctx->lzo.block.uncompressed_length) {
			// Bytes left in block, seek to min(bytesleft, end of block)
			uint16_t bytes = min(bytesleft, ctx->lzo.block.uncompressed_length - ctx->lzo.block.uncompressed_offset);
			ctx->lzo.block.uncompressed_offset += bytes;
			position += bytes;
			continue;
		}

		if (bytesleft > ctx->lzo.header.blocksize) {
			// Seek beyond current block
			if (!pfs_skiplzoblock(ctx)) {
				errnogoto(EIO, seekerr);
			}

			position += ctx->lzo.header.blocksize;
			continue;
		}

		// Seek position within one block. Load another block here and continue
		if (!pfs_readlzoblock(ctx)) {
			errnogoto(EIO, seekerr);
		}
	}

	return offset;

seekerr:
	pfs_error(ctx) = true;
	return -1;
}

bool pfs_initlzo(void) {
	return lzo_init() == LZO_E_OK;
}

#endif
