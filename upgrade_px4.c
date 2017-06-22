#include "upgrade_px4.h"

#ifdef _POSIX_VERSION
#include "posix/functions.h"
#include "posix/helper_functions.h"
#endif

#include "base64.h"
#include "zlib.h"

#define CHUNK 16384

static char *json_tag_content_start(const char *json, const char *tag)
{
    char *start = strstr(json, tag);
    if (start == NULL) {
        fprintf(stderr, "no %s\n", tag);
        return NULL;
    }
    start = strchr(start, ':');
    if (start == NULL) {
        fprintf(stderr, "no :\n");
        return NULL;
    }

    start = strchr(start, '"');
    if (start == NULL) {
        fprintf(stderr, "no quote");
        return NULL;
    }
    return start+1;
}

/*
  handle ardupilot fw update
 */

#define ZLIB_CHUNKSIZE 16384
struct readstate {
    unsigned offset;

    const char *base64_src;
    char base64_decoded_bytes[3];
    unsigned base64_decoded_bytecount;
    unsigned base64_decoded_nextbyte;

    z_stream strm;
    unsigned char zlib_out[ZLIB_CHUNKSIZE];
    unsigned zlib_out_bytecount;

    unsigned strm_next_out_nextbyte;

};

static void my_read_decode_more_base64(struct readstate *my_state)
{
    char quartet[4];
    unsigned qi = 0;
    fprintf(stderr, "offset=%u\n", my_state->offset);
    while (qi<4 && my_state->base64_src[my_state->offset] != '"') {
        quartet[qi] = my_state->base64_src[my_state->offset];
        my_state->offset++;
        if (quartet[qi] == '=') {
            break;
        }
        qi++;
    }
    int decoded_bytes;
    if (qi != 4) {
        decoded_bytes = base64_decode_tail(my_state->base64_decoded_bytes, quartet, qi);
    } else {
        decoded_bytes = base64_decode_quartet(my_state->base64_decoded_bytes, quartet);
        if (decoded_bytes == 0) {
            decoded_bytes = 3;
        }
    }
    if (decoded_bytes == -1) {
        return;
    }
    my_state->base64_decoded_bytecount = (unsigned)decoded_bytes;
}

static void my_read_inflate_base64(struct readstate *my_state)
{
    while (my_state->strm.avail_out == ZLIB_CHUNKSIZE) {
        fprintf(stderr, "iz zero\n");
        if (my_state->base64_decoded_bytecount == my_state->base64_decoded_nextbyte) {
            my_read_decode_more_base64(my_state);
            if (my_state->base64_decoded_bytecount == 0) {
                // no more base64 data...
                fprintf(stderr, "no more base64 data - all done inflating\n");
                (void)inflateEnd(&my_state->strm);
                return;
            }
            my_state->base64_decoded_nextbyte = 0;
        }

        my_state->strm.avail_in = 1;
        my_state->strm.next_in = &my_state->base64_decoded_bytes[my_state->base64_decoded_nextbyte];
        fprintf(stderr, "feeding 0x%02x\n", my_state->strm.next_in[0]);
        my_state->base64_decoded_nextbyte++;

        int ret = inflate(&my_state->strm, Z_NO_FLUSH);
        fprintf(stderr, "after inflate: %u (%u)\n", my_state->strm.avail_in, ZLIB_CHUNKSIZE-my_state->strm.avail_out);
        if (ret == Z_STREAM_ERROR) {
            abort();
        }
        switch (ret) {
        case Z_STREAM_END:
            fprintf(stderr, "end\n");
        case Z_NEED_DICT:
            fprintf(stderr, "dict\n");
        case Z_DATA_ERROR:
            fprintf(stderr, "data error\n");
        case Z_MEM_ERROR:
            fprintf(stderr, "mem error\n");
            (void)inflateEnd(&my_state->strm);
            return;
        }
    }
    fprintf(stderr, "have data\n");
}



static int my_read(void *_my_state, char *ret, const int len)
{
    fprintf(stderr, "my_read called for (%u) bytes\n", len);
    struct readstate *my_state = (struct readstate*)_my_state;
    int bytes_read = 0;
    while (bytes_read < len) {
        if (ZLIB_CHUNKSIZE-my_state->strm.avail_out == my_state->strm_next_out_nextbyte) {
            // no bytes currently available
            my_read_inflate_base64(my_state);
            if (ZLIB_CHUNKSIZE-my_state->strm.avail_out == my_state->strm_next_out_nextbyte) {
                fprintf(stderr, "Break\n");
                break;
            }
            my_state->strm_next_out_nextbyte = 0;
        }
        ret[bytes_read] = my_state->zlib_out[my_state->strm_next_out_nextbyte];
        fprintf(stderr, "Got byte 0x%02x\n", ret[bytes_read]);
        my_state->strm_next_out_nextbyte++;

        bytes_read++;
    }
    return bytes_read;
}

void handle_ardupilot_upgrade_px4(struct template_state *tmpl, const char *filename, const char *filedata, uint32_t size)
{
    console_printf("ardupilot_upgrade: size=%u\n", size);
    set_upload_progress(1);
    uint32_t fw_offset;
    if (filedata[0] != '{') {
        fprintf(stderr, "no brace\n");
        abort();
    }
    fprintf(stderr, "brace\n");
    // I'm thoroughly convinced this is a JSON file upload...
    char *start = json_tag_content_start(filedata, "image");
    fprintf(stderr, "start=%p\n", start);
    if (start == NULL) {
        set_upload_message("Bad upload");
        return;
    }
    fw_offset = start-filedata;
    fprintf(stderr, "offset=%u\n", fw_offset);
    struct readstate my_state;
    memset(&my_state, '\0', sizeof(my_state));
    my_state.base64_src = start;
    my_state.offset = 0;

    /* allocate inflate state */
    my_state.strm.zalloc = Z_NULL;
    my_state.strm.zfree = Z_NULL;
    my_state.strm.opaque = Z_NULL;
    my_state.strm.avail_in = 0;
    /* my_state.strm.next_in = &zlib_in; */
    my_state.strm.next_out = my_state.zlib_out;
    my_state.strm.avail_out = ZLIB_CHUNKSIZE;
    if (inflateInit(&my_state.strm) != Z_OK) {
        return;
    }

    upgrade_fc_firmware(my_read, (void*)&my_state);
}
