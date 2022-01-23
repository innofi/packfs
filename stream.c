#include <string.h>

#include <esp_err.h>
#include <esp_log.h>

#include "packfs-priv.h"


#if !defined(CONFIG_PACKFS_STREAM_SUPPORT)
#error "This file should NOT be included if CONFIG_PACKFS_STREAM_SUPPORT is not set."
#elif !defined(CONFIG_PACKFS_PROCESS_SUPPORT)
#error "Necessary dependencies not enabled. Needs CONFIG_PACKFS_PROCESS_SUPPORT."
#else

void * pfss_extra(pfs_stream_t * stream) {
	return stream != NULL? &stream->buffer[stream->size] : NULL;
}

pfs_proc_t * pfss_create(size_t buffersize, pfsp_io_t * ios, packfs_proccb_t * cbs, void * userdata, size_t extrasize) {
	// Sanity check args
	if unlikely(buffersize < PACKFS_MIN_STREAMSIZE) {
		return NULL;
	}

	// Allocate and set up proc + stream
	pfs_proc_t * proc = pfsp_malloc(userdata, PP_STREAM, ios, cbs, cbs->onbodyhash != NULL || cbs->onimgentryend != NULL, sizeof(pfs_stream_t) + buffersize + extrasize);
	pfs_stream_t * stream = pfsp_extra(proc);
	if (proc == NULL || stream == NULL) {
		return NULL;
	}
	stream->size = buffersize;
	stream->offset = 0;
	stream->length = 0;
	stream->eof = false;

	return proc;
}

packfs_status_t pfss_read(pfs_proc_t * proc, void * data, size_t minlength, size_t maxlength, size_t * outlength) {
	pfs_stream_t * stream = pfsp_extra(proc);

	if (stream->offset == 0 && stream->eof) {
		//ESP_LOGI(PACKFS_TAG, ">> EOF");
		return PS_EOF;

	} else if (stream->length < minlength) {
		//ESP_LOGI(PACKFS_TAG, ">> AGAIN");
		return PS_AGAIN;

	} else {

		// Circular buffer
		size_t bytes = min(stream->length, maxlength);
		size_t chunk1 = min(bytes, stream->size - stream->offset);
		size_t chunk2 = bytes - chunk1;

		memcpy(data, &stream->buffer[stream->offset], chunk1);
		if (chunk2 > 0) memcpy((uint8_t *)data + chunk1, &stream->buffer[0], chunk2);

		stream->offset = (stream->offset + bytes) % stream->size;
		stream->length -= bytes;
		*outlength = bytes;

		//ESP_LOGI(PACKFS_TAG, ">%zu", bytes);
		return PS_OK;
	}
}

ssize_t packfs_stream_load(packfs_stream_t stream, void * data, size_t length) {
	pfs_proc_t * proc = (pfs_proc_t *)stream;
	pfs_stream_t * extra = NULL;

	// Sanity check
	if unlikely(proc == NULL || proc->type != PP_STREAM || (extra = pfsp_extra(proc)) == NULL) {
		return -1;
	}

	size_t bytes = min(length, extra->size - extra->length);
	if (extra->eof || bytes == 0) {
		// Buffer can take no more bytes
		return 0;
	}

	// Circular buffer logic
	size_t start1 = (extra->offset + extra->length) % extra->size;
	size_t chunk1 = min(bytes, extra->size - start1);
	memcpy(&extra->buffer[start1], data, chunk1);

	// Handle wrap-around
	size_t start2 = (start1 + chunk1) % extra->size;
	size_t chunk2 = bytes - chunk1;
	if (chunk2 > 0)	memcpy(&extra->buffer[start2], (uint8_t *)data + chunk1, chunk2);

	extra->length += bytes;
	return bytes;
}

packfs_status_t packfs_stream_process(packfs_stream_t stream) {
	pfs_proc_t * proc = (pfs_proc_t *)stream;

	// Sanity check
	if unlikely(proc == NULL || proc->type != PP_STREAM) {
		return PS_FAIL;
	}

	return pfsp_process(proc);
}

packfs_status_t packfs_stream_loadandprocess(packfs_stream_t stream, void * data, size_t length) {
	labels(procerr); // @suppress("Type cannot be resolved")

	packfs_status_t status = PS_OK;
	size_t offset = 0;
	while (offset < length && (status == PS_OK || status == PS_AGAIN)) {
		ssize_t bytes = packfs_stream_load(stream, (uint8_t *)data + offset, length - offset);
		if (bytes < 0) {
			// Load failed
			goto procerr;

		} else if (bytes == 0 && status == PS_AGAIN) {
			// Full buffer but can't read
			goto procerr;
		}

		offset += bytes;

		status = PS_OK;
		while (status == PS_OK) {
			status = packfs_stream_process(stream);
		}
	}

	if (offset < length) {
		// Couldn't write all data
		return PS_FAIL;
	}

	return status;

procerr:
	return PS_FAIL;
}

packfs_status_t packfs_stream_loadeof(packfs_stream_t stream) {
	pfs_proc_t * proc = (pfs_proc_t *)stream;

	// Sanity check
	if unlikely(proc == NULL || proc->type != PP_STREAM) {
		return PS_FAIL;
	}

	// Set EOF flag
	pfs_stream_t * extra = pfsp_extra(proc);
	extra->eof = true;

	return PS_OK;
}

packfs_status_t packfs_stream_flush(packfs_stream_t stream) {
	pfs_proc_t * proc = (pfs_proc_t *)stream;

	// Sanity check
	if unlikely(proc == NULL || proc->type != PP_STREAM) {
		return PS_FAIL;
	}

	packfs_status_t status = PS_OK;
	while (status == PS_OK) {
		status = packfs_stream_process(stream);
	}

	return status;
}

packfs_status_t packfs_stream_loadeofandflush(packfs_stream_t stream) {
	packfs_status_t status = PS_OK;

	if ((status = packfs_stream_loadeof(stream)) != PS_OK) {
		return status;
	}

	status = packfs_stream_flush(stream);
	if (status == PS_OK || status == PS_AGAIN) {
		// We should be seeing PS_EOF, PS_OK/PS_AGAIN is a failure
		status = PS_FAIL;
	}

	return status;
}


esp_err_t packfs_stream_tofile(FILE * fp, size_t bufsize, packfs_proccb_t * cbs, void * userdata, packfs_stream_t * out_stream) {
	if unlikely(fp == NULL || cbs == NULL || out_stream == NULL) {
		return ESP_ERR_INVALID_ARG;
	}
	if unlikely(bufsize < PACKFS_MIN_STREAMSIZE) {
		return ESP_ERR_INVALID_SIZE;
	}

	// Allocate and proc structure
	pfsp_io_t ios = {
		.read = pfss_read,
		.write = pfsp_tofile_write
	};
	pfs_proc_t * proc = pfss_create(bufsize, &ios, cbs, userdata, 0);
	if (proc == NULL) {
		return ESP_ERR_NO_MEM;
	}

	proc->ctx.backing = fp;
	*out_stream = (packfs_stream_t)proc;
	return ESP_OK;
}

esp_err_t packfs_stream_tofile_close(packfs_stream_t stream) {
	pfs_proc_t * proc = (pfs_proc_t *)stream;

	if unlikely(proc == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	// Return status
	esp_err_t err = ESP_OK;

	// Push EOF if necessary
	if (proc->state != PS_CLOSED && packfs_stream_loadeofandflush(stream) != PS_EOF) {
		err = ESP_FAIL;
	}

	pfsp_close(proc);
	pfsp_free(proc);
	return err;
}

#endif
