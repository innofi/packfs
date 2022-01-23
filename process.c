#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <esp_err.h>
#include <esp_log.h>

#include "packfs-priv.h"

#ifndef CONFIG_PACKFS_PROCESS_SUPPORT
#error "This file should NOT be included if CONFIG_PACKFS_PROCESS_SUPPORT is not set."
#else

void * pfsp_extra(pfs_proc_t * proc) {
	return proc != NULL? proc->extra : NULL;
}

pfs_proc_t * pfsp_malloc(void * userdata, pfsp_type_t type, pfsp_io_t * ios, packfs_proccb_t * cbs, bool hashmem, size_t extrasize) {
	// Check args
	if unlikely(ios == NULL || ios->read == NULL) {
		return NULL;
	}

	// Allocate memory
	pfs_proc_t * proc = calloc(1, sizeof(pfs_proc_t) + extrasize + (hashmem? sizeof(mbedtls_sha256_context) : 0));
	if (proc == NULL) {
		return NULL;
	}

	// Initialize internal vars
	proc->type = type;
	proc->section = PS_HEADER;
	proc->state = PS_READHEADER;
	proc->userdata = userdata;

	// Copy cbs
	memcpy(&proc->ios, ios, sizeof(pfsp_io_t));
	if (cbs != NULL) memcpy(&proc->cbs, cbs, sizeof(packfs_proccb_t));

	// Hook up sha256 context
	if (hashmem) {
		proc->shactx = (void *)&((uint8_t *)proc)[sizeof(pfs_proc_t) + extrasize];
		mbedtls_sha256_init(proc->shactx);
	}

	return proc;
}

void pfsp_free(pfs_proc_t * proc) {
	if unlikely(proc == NULL) return;

	// Free entries
	if (proc->entries != NULL) {
		free(proc->entries);
		proc->entries = NULL;
	}

#ifdef CONFIG_PACKFS_LZO_SUPPORT
	// Free compression space
	pfs_lzofree(&proc->ctx);
#endif

	// Free the sha256 ctx
	if (proc->shactx != NULL) {
		mbedtls_sha256_free(proc->shactx);
		proc->shactx = NULL;
	}

	// Free the base object
	free(proc);
}

packfs_status_t pfsp_process(pfs_proc_t * proc) {
#define callback(name, ...)		({ if (proc->cbs.name != NULL) proc->cbs.name(proc->userdata, ##__VA_ARGS__); })
#define callbackr(name, ...)	({ bool r = (proc->cbs.name != NULL)? proc->cbs.name(proc->userdata, ##__VA_ARGS__) : true; r; })
#define errorreturn(err)		({ callback(onerror, __FILE__, __LINE__, proc->section, (err)); pfsp_close(proc); return PS_FAIL; })

#define wantskip(filesize)		(\
									((ctx->entry.flags & PT_REG) && proc->cbs.onentrystart == NULL && proc->cbs.onentrydata == NULL && proc->cbs.onregentryend == NULL) || \
									((ctx->entry.flags & PT_IMG) && proc->cbs.onentrystart == NULL && proc->cbs.onentrydata == NULL && proc->cbs.onimgentryend == NULL) || \
									(proc->cbs.onentrystart != NULL && !proc->cbs.onentrystart(proc->userdata, &ctx->entry, filesize)) \
								)

#define addhash(enable)			({ if ((enable) && mbedtls_sha256_update_ret(proc->shactx, readbuffer, bytes) != 0) errorreturn(EBADMSG); })
#define wanthash_head()			(proc->shactx != NULL && proc->cbs.onbodyhash != NULL)
#define wanthash_body()			(proc->shactx != NULL && proc->section == PS_REGENTRY && proc->cbs.onbodyhash != NULL)
#define wanthash_img()			(proc->shactx != NULL && proc->section == PS_IMGENTRY && (ctx->entry.flags & PT_IMG) && proc->cbs.onimgentryend != NULL)

	packfs_status_t status = PS_OK;
	pfs_ctx_t * ctx = &proc->ctx;
	uint8_t tmpbuffer[PACKFS_PROC_BUFSIZE];

	while (status == PS_OK) {
		// Determine the sizes to read
		size_t readmin = 0, readmax = 0;
		void * readbuffer = NULL;
		switch (proc->state) {
			case PS_READHEADER: {
				// Must read in entire header
				readmin = readmax = sizeof(packfs_header_t);
				readbuffer = &proc->header;
				break;
			}
			case PS_READMETA: {
				// Must read in one meta struct at a time
				readmin = readmax = sizeof(packfs_meta_t);
				readbuffer = &ctx->meta;
				break;
			}
			case PS_READINDEX: {
				// Read the index section
				size_t start = sizeof(packfs_header_t) + proc->header.metasize;
				readmin = 1;
				readmax = start + proc->header.indexsize - ctx->offset;
				readbuffer = ((uint8_t *)proc->entries) + (ctx->offset - start);
				break;
			}
			case PS_READENTRY: {
				// Nothing to read here, continue process at bottom
				readmin = readmax = 0;
				break;
			}
			case PS_READIMGHASH: {
				readmin = readmax = PACKFS_HASHSIZE;

				// Use header.packhash (in header section) to store img hash. This is not ideal, but we need
				// a place to store the hash, and I don't want to allocate another 32 bytes somewhere.
				// When we get to this point, header.packhash isn't needed anymore
				readbuffer = proc->section == PS_IMGENTRY? proc->header.packhash : tmpbuffer;
				break;
			}
			case PS_SKIPENTRY:
			case PS_READREGCHUNK: {
				// Read as much as possible up to end of entry
				readmin = 1;
				readmax = min(PACKFS_PROC_BUFSIZE, ctx->entry.length - (ctx->offset - ctx->entry.offset));
				readbuffer = tmpbuffer;
				break;
			}
#ifdef CONFIG_PACKFS_LZO_SUPPORT
			case PS_READLZOHEADER: {
				// Read in lzoheader
				readmin = readmax = sizeof(pfs_lzoheader_t);
				readbuffer = &ctx->lzo.header;
				break;
			}
			case PS_READLZOSIZE: {
				// Read in size of block
				readmin = readmax = sizeof(uint16_t);
				readbuffer = &ctx->lzo.block.compressed_length;
				break;
			}
			case PS_READLZOCHUNK: {
				// Read in as much as possible up to block size
				readmin = 1;
				readmax = ctx->lzo.block.compressed_length - ctx->lzo.block.uncompressed_offset;
				readbuffer = &ctx->lzo.block.compressed[ctx->lzo.block.uncompressed_offset];
				break;
			}
#else
			case PS_READLZOHEADER:{
				readmin = readmax = 1;
				readbuffer = tmpbuffer;
				break;
			}
			case PS_READLZOSIZE:
			case PS_READLZOCHUNK: {
				errorreturn(EPROTO);
			}
#endif
			case PS_CLOSED: {
				// Nothing to read here, continue process at bottom
				readmin = readmax = 0;
				break;
			}
		}

		// Read the bytes in
		uint32_t bytes = 0;
		if (readmax > 0) {
			status = proc->ios.read(proc, readbuffer, readmin, readmax, &bytes);

			if (status == PS_OK && bytes < readmin) {
				// Invalid state, must read at least readmin to have status PS_OK
				status = PS_FAIL;
			}
		}

		// Stop if we weren't able to read necessary amount
		if (status != PS_OK) {
			if (status == PS_FAIL) errorreturn(EIO);
			break;
		}

		// Update offset pointer
		ctx->offset += bytes;

		// Handle callbacks and state change
		switch (proc->state) {
			case PS_READHEADER: {
				// Verify data
				if (!pfs_checkheader(&proc->header)) {
					errorreturn(EFTYPE);
				}

				// Allocate extry index size
				if ((proc->entries = calloc(proc->header.indexsize / sizeof(packfs_entry_t), sizeof(packfs_entry_t))) == NULL) {
					errorreturn(ENOMEM);
				}

				// Call cb
				callback(onheader, &proc->header);

				// Setup hash
				if (wanthash_head() && mbedtls_sha256_starts_ret(proc->shactx, 0) != 0) {
					errorreturn(EBADMSG);
				}

				// Advance state
				proc->section = PS_META;
				proc->state = PS_READMETA;
				break;
			}
			case PS_READMETA: {
				// Call cb
				callback(onmeta, &ctx->meta);

				// Add bytes to hash
				addhash(wanthash_head());

				// Advance state
				if (ctx->offset == (sizeof(packfs_header_t) + proc->header.metasize)) {
					proc->section = PS_INDEX;
					proc->state = PS_READINDEX;
				}
				break;
			}
			case PS_READINDEX: {
				// Add bytes to hash
				addhash(wanthash_head());

				// Advance state
				if (ctx->offset == (sizeof(packfs_header_t) + proc->header.metasize + proc->header.indexsize)) {
					proc->section = PS_REGENTRY;
					proc->state = PS_READENTRY;
				}
				break;
			}
			case PS_READENTRY: {
				// Complete body hash if needed
				size_t bodysize = sizeof(packfs_header_t) + proc->header.metasize + proc->header.indexsize + proc->header.regentrysize;
				if (ctx->offset == bodysize) {
					uint8_t * calchash = NULL;

					if (wanthash_body()) {
						calchash = tmpbuffer;
						if (mbedtls_sha256_finish_ret(proc->shactx, tmpbuffer) != 0) {
							errorreturn(EBADMSG);
						}
					}

					bool matches = calchash != NULL && memcmp(proc->header.packhash, calchash, PACKFS_HASHSIZE) == 0;
					if (!callbackr(onbodyhash, proc->header.packhash, calchash, matches)) {
						status = calchash != NULL && !matches? PS_HASHNOMATCH : PS_USERBAIL;
						break;
					}
				}

				// Determine if we're EOF
				if (ctx->offset == (bodysize + proc->header.imgentrysize)) {
					status = PS_EOF;
					break;
				}

				// Load entry
				memcpy(&ctx->entry, &proc->entries[proc->onentry], sizeof(packfs_entry_t));

				// Determine section
				bool imgsection = ctx->offset >= (sizeof(packfs_header_t) + proc->header.metasize + proc->header.indexsize + proc->header.regentrysize);
				proc->section = imgsection? PS_IMGENTRY : PS_REGENTRY;

				// Start new hash if img entry
				if (wanthash_img() && mbedtls_sha256_starts_ret(proc->shactx, 0) != 0) {
					errorreturn(EBADMSG);
				}

				// Advance state
				if (ctx->entry.flags & PT_IMG) {
					proc->state = PS_READIMGHASH;
				} else{
					proc->state = (ctx->entry.flags & PF_LZO)? PS_READLZOHEADER : PS_READREGCHUNK;
				}

				break;
			}
			case PS_READIMGHASH: {
				// Add bytes to hash
				addhash(wanthash_body());

				// Advance state
				proc->state = (ctx->entry.flags & PF_LZO)? PS_READLZOHEADER : PS_READREGCHUNK;
				break;
			}
			case PS_SKIPENTRY: {
				// Add bytes to hash
				addhash(wanthash_body());

				// Advance state
				if (ctx->offset == (ctx->entry.offset + ctx->entry.length)) {
					proc->onentry += 1;
					proc->state = PS_READENTRY;
				}
				break;
			}
			case PS_READREGCHUNK: {
				// Add bytes to hash
				addhash(wanthash_body() || wanthash_img());

				// Determine if this is the first read of section
				uint32_t start = ctx->entry.offset + ((ctx->entry.flags & PT_IMG)? PACKFS_HASHSIZE : 0);

				// See if user wants to skip this section
				if ((ctx->offset - bytes) == start && wantskip(ctx->entry.length - ((ctx->entry.flags & PT_IMG)? PACKFS_HASHSIZE : 0))) {
					proc->state = PS_SKIPENTRY;
					break;
				}

				callback(onentrydata, &ctx->entry, readbuffer, bytes, ctx->offset - bytes - start);

				// Handle end-of-entry
				if (ctx->offset == (ctx->entry.offset + ctx->entry.length)) {
					if (proc->section == PS_IMGENTRY && (ctx->entry.flags & PT_IMG)) {
						uint8_t * calchash = NULL;

						// Calculate the hash of the image
						if (wanthash_img()) {
							calchash = tmpbuffer;
							if (mbedtls_sha256_finish_ret(proc->shactx, calchash) != 0) {
								errorreturn(EBADMSG);
							}
						}

						// Handle callback
						bool matches = calchash != NULL && memcmp(proc->header.packhash, calchash, PACKFS_HASHSIZE) == 0;
						if (!callbackr(onimgentryend, &ctx->entry, proc->header.packhash, calchash, matches)) {
							status = calchash != NULL && !matches? PS_HASHNOMATCH : PS_USERBAIL;
							break;
						}

					} else {
						// Regular entry
						if (!callbackr(onregentryend, &ctx->entry)) {
							status = PS_USERBAIL;
							break;
						}
					}

					// Advance state
					proc->onentry += 1;
					proc->state = PS_READENTRY;
				}
				break;
			}
#ifdef CONFIG_PACKFS_LZO_SUPPORT
			case PS_READLZOHEADER: {
				// Add bytes to hash
				addhash(wanthash_body());

				// Check the header
				if (!pfs_checklzoheader(ctx)) {
					errorreturn(EINVAL);
				}

				// See if user wants to skip this section
				if (wantskip(ctx->lzo.header.uncompressed_length)) {
					proc->state = PS_SKIPENTRY;
					break;
				}

				// Allocate the header sizes
				if (!pfs_preplzo(ctx) || !pfs_lzomalloc(ctx)) {
					errorreturn(ENOMEM);
				}

				// Advance state
				proc->state = PS_READLZOSIZE;
				break;
			}
			case PS_READLZOSIZE: {
				// Add bytes to hash
				addhash(wanthash_body());

				// Check the block
				if (!pfs_checklzoblock(ctx)) {
					errorreturn(EINVAL);
				}

				// Advance state
				proc->state = PS_READLZOCHUNK;
				break;
			}
			case PS_READLZOCHUNK: {
				// Add bytes to hash
				addhash(wanthash_body());

				// Increment offset counter
				ctx->lzo.block.uncompressed_offset += bytes;

				// Execute decompress when we've read in full block
				if (ctx->lzo.block.uncompressed_offset == ctx->lzo.block.compressed_length) {
					// Determine offset into entry
					uint32_t offset = ctx->lzo.numblocks * ctx->lzo.header.blocksize;

					// Decompress block
					if (!pfs_decompresslzoblock(ctx)) {
						errorreturn(EINVAL);
					}

					// Callback
					callback(onentrydata, &ctx->entry, ctx->lzo.block.uncompressed, ctx->lzo.block.uncompressed_length, offset);

					// Add bytes to hash
					if (wanthash_img() && mbedtls_sha256_update_ret(proc->shactx, ctx->lzo.block.uncompressed, ctx->lzo.block.uncompressed_length) != 0) {
						errorreturn(EBADMSG);
					}

					// Determine if we're at end of file
					if ((offset + ctx->lzo.block.uncompressed_length) == ctx->lzo.header.uncompressed_length) {
						if (proc->section == PS_IMGENTRY && (ctx->entry.flags & PT_IMG)) {
							uint8_t * calchash = NULL;

							// Calculate the hash of the image
							if (wanthash_img()) {
								calchash = tmpbuffer;
								if (mbedtls_sha256_finish_ret(proc->shactx, calchash) != 0) {
									errorreturn(EBADMSG);
								}
							}

							// Handle callback
							bool matches = calchash != NULL && memcmp(proc->header.packhash, calchash, PACKFS_HASHSIZE) == 0;
							if (!callbackr(onimgentryend, &ctx->entry, proc->header.packhash, calchash, matches)) {
								status = calchash != NULL && !matches? PS_HASHNOMATCH : PS_USERBAIL;
								break;
							}
						} else {
							// Regular entry
							if (!callbackr(onregentryend, &ctx->entry)) {
								status = PS_USERBAIL;
								break;
							}
						}

						// Advance state to next entry
						proc->onentry += 1;
						proc->state = PS_READENTRY;

					} else {
						// Advance state to next block
						proc->state = PS_READLZOSIZE;
					}
				}
				break;
			}
			case PS_CLOSED: {
				// Invalid state, this is bad
				status = PS_FAIL;
				break;
			}
#else
			case PS_READLZOHEADER: {
				// Add bytes to hash
				addhash(wanthash_body());

				// See if user wants to skip this section
				if (wantskip(0)) {
					proc->state = PS_SKIPENTRY;
					break;
				}

				// Can't read any more, fault
				errorreturn(EPROTO);
				break;
			}
			default: errorreturn(EFAULT);
#endif
		}

		// Write the bytes out
		if (bytes > 0 && proc->ios.write != NULL && proc->ios.write(proc, readbuffer, bytes) != PS_OK) {
			errorreturn(EIO);
		}
	}

	// Verify proper EOF
	if (status == PS_EOF && proc->state != PS_READENTRY && proc->state != PS_READIMGHASH) {
		errorreturn(EPIPE);
	}

	// Callback EOF
	if (status == PS_EOF) {
		if (!callbackr(oneof)) status = PS_USERBAIL;
		pfsp_close(proc);
	}

	return status;
}

void pfsp_close(pfs_proc_t * proc) {
	// Sanity check
	if unlikely(proc == NULL || proc->state == PS_CLOSED) {
		return;
	}

	// Call back eof handler
	/*if (proc->ios.close != NULL) {
		proc->ios.close(proc);
	}*/

	proc->state = PS_CLOSED;
}

packfs_status_t pfsp_fromfile_read(pfs_proc_t * proc, void * data, size_t minlength, size_t maxlength, size_t * outlength) {
	// Since we're reading from a file with all the data, we should be able to read maxlength
	size_t read = fread(data, maxlength, 1, proc->ctx.backing);
	if (read == 0 && feof(proc->ctx.backing) != 0) {
		//ESP_LOGI(PACKFS_TAG, ">> EOF");
		return PS_EOF;

	} else if (read == 1) {
		//ESP_LOGI(PACKFS_TAG, ">%zu", maxlength);
		*outlength = maxlength;
		return PS_OK;

	} else {
		//ESP_LOGE(PACKFS_TAG, ">> BAD READ");
		return PS_FAIL;
	}
}

packfs_status_t pfsp_tofile_write(pfs_proc_t * proc, void * data, size_t length) {
	return fwrite(data, length, 1, proc->ctx.backing) == 1? PS_OK : PS_FAIL;
}

esp_err_t packfs_process_fromfile(const char * filepath, packfs_proccb_t * cbs, void * userdata) {
	labels(procerr); // @suppress("Type cannot be resolved")

	// Check args
	if unlikely(filepath == NULL || cbs == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	// Allocate and proc structure
	pfsp_io_t ios = {
		.read = pfsp_fromfile_read
	};
	pfs_proc_t * proc = pfsp_malloc(userdata, PP_FILE, &ios, cbs, cbs->onbodyhash != NULL || cbs->onimgentryend != NULL, 0);
	if (proc == NULL) {
		return ESP_ERR_NO_MEM;
	}

	// Return err
	esp_err_t err = ESP_OK;

	// Open backing file
	if ((proc->ctx.backing = fopen(filepath, "r")) == NULL) {
		errgoto(ESP_FAIL, procerr);
	}

	// Do the processing
	if (pfsp_process(proc) != PS_EOF || proc->state != PS_CLOSED) {
		err = ESP_FAIL;
	}

	// Close the spiffs file
	fclose(proc->ctx.backing);
	proc->ctx.backing = NULL;

procerr:
	pfsp_free(proc);
	return err;
}

void packfs_process_free(packfs_process_t proc) {
	pfsp_free((pfs_proc_t *)proc);
}

#endif
