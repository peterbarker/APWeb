#include "upgrade_px4.h"

#ifdef _POSIX_VERSION
#include "posix/functions.h"
#include "posix/helper_functions.h"
#endif

#include "base64.h"
#include "zlib.h"
#include "md5.h"

#define CHUNK 16384

static char *json_tag_value_start(const char *json, const char *tag)
{
    char quoted_tag[64];
    snprintf(quoted_tag, sizeof(quoted_tag), "\"%s\"", tag);
    char *start = strstr(json, quoted_tag);
    if (start == NULL) {
        fprintf(stderr, "no %s\n", tag);
        return NULL;
    }

    start = strchr(start, ':');
    if (start == NULL) {
        fprintf(stderr, "no :\n");
        return NULL;
    }
    start += 1; // skip the colon

    // skip whitespace
    while (*start != '\0' && isspace(*start)) {
        start++;
    }

    return (start);
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
    /* fprintf(stderr, "offset=%u (%x)\n", my_state->offset, my_state->base64_src[my_state->offset]); */
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
    while (ZLIB_CHUNKSIZE-my_state->strm.avail_out == my_state->strm_next_out_nextbyte) {
        if (my_state->base64_decoded_bytecount == my_state->base64_decoded_nextbyte) {
            my_read_decode_more_base64(my_state);
            if (my_state->base64_decoded_bytecount == 0) {
                // no more base64 data...
                (void)inflateEnd(&my_state->strm);
                return;
            }
            my_state->base64_decoded_nextbyte = 0;
        }

        my_state->strm.avail_in = 1;
        my_state->strm.next_in = (unsigned char*)&my_state->base64_decoded_bytes[my_state->base64_decoded_nextbyte];
        /* fprintf(stderr, "feeding 0x%02x\n", my_state->strm.next_in[0]); */
        my_state->base64_decoded_nextbyte++;

        int ret = inflate(&my_state->strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR) {
            abort();
        }
        switch (ret) {
        case Z_STREAM_END:
            (void)inflateEnd(&my_state->strm);
            return;
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
}



static int my_read(void *_my_state, char *ret, const int len)
{
    struct readstate *my_state = (struct readstate*)_my_state;
    int bytes_read = 0;
    while (bytes_read < len) {
        if (ZLIB_CHUNKSIZE-my_state->strm.avail_out == my_state->strm_next_out_nextbyte) {
            // no bytes currently available
            my_state->strm.avail_out = ZLIB_CHUNKSIZE;
            my_state->strm.next_out = my_state->zlib_out;
            my_state->strm_next_out_nextbyte = 0;
            my_read_inflate_base64(my_state);
            if (ZLIB_CHUNKSIZE-my_state->strm.avail_out == 0) {
                break;
            }
            /* fprintf(stderr, "Deflated %u bytes\n", ZLIB_CHUNKSIZE-my_state->strm.avail_out); */
        }
        ret[bytes_read] = my_state->zlib_out[my_state->strm_next_out_nextbyte];
        /* fprintf(stderr, "Got byte 0x%02x\n", (uint8_t)ret[bytes_read]); */
        my_state->strm_next_out_nextbyte++;

        bytes_read++;
    }
    return bytes_read;
}

static void init_mystate(struct readstate *my_state)
{
    memset(my_state, '\0', sizeof(*my_state));
    my_state->offset = 0;

    /* allocate inflate state */
    my_state->strm.zalloc = Z_NULL;
    my_state->strm.zfree = Z_NULL;
    my_state->strm.opaque = Z_NULL;
    my_state->strm.avail_in = 0;
    /* my_state->strm.next_in = &zlib_in; */
    my_state->strm.next_out = my_state->zlib_out;
    my_state->strm.avail_out = ZLIB_CHUNKSIZE;
    if (inflateInit(&my_state->strm) != Z_OK) {
        return;
    }
}


static void dump_md5(const char *name, const unsigned char md5[16])
{
    fprintf(stderr, "%s: ", name);
    for (unsigned i=0; i<16; i++) {
        fprintf(stderr, "%02x", (unsigned char)md5[i]);
    }
    fprintf(stderr, "\n");
}

static void fc_firmware_md5(int(read)(void*,char*,int), void *readstate, unsigned char calculated_md5[16], unsigned long *total_length)
{
    char buffer[128];

    struct MD5Context ctx;
    MD5Init(&ctx);
    unsigned total_read = 0;
    while (true) {
        int num_read = read(readstate, buffer, sizeof(buffer));
        total_read += num_read;
        /* printf("read %u bytes (total=%u)\n", num_read, total_read); */
        if (num_read == 0) {
            break;
        }
        /* for (unsigned int i=0; i<num_read;i++) { */
        /*     fprintf(stderr, "%c", buffer[i]); */
        /* } */
        MD5Update(&ctx, (unsigned char*)buffer, num_read);
    }

    MD5Final(calculated_md5, &ctx);
    *total_length = total_read;
}
static void fc_firmware_length(int(read)(void*,char*,int), void *readstate, unsigned long *total_length)
{
    char buffer[128];

    unsigned total_read = 0;
    while (true) {
        int num_read = read(readstate, buffer, sizeof(buffer));
        total_read += num_read;
        if (num_read == 0) {
            break;
        }
    }

    *total_length = total_read;
}

static int fc_find_md5_in_json(unsigned char supplied_md5[16], const char *filedata)
{
    char *start = json_tag_value_start(filedata, "image_md5");
    if (start == NULL) {
        set_upload_message("No MD5");
        return -1;
    }
    start += 1; // skip opening quote
    for (unsigned i=0; i<32; i+=2) {
        const unsigned char x = start[i];
        const unsigned char y = start[i+1];
        const unsigned char x1 = (x >= 'a') ? (10+x-'a') : (x-'0');
        const unsigned char y1 = (y >= 'a') ? (10+y-'a') : (y-'0');
        supplied_md5[i/2] = x1<<4 | y1;
    }
    return 0;
}

static int fc_find_image_size_in_json(unsigned int *supplied_size, const char *filedata)
{
    char *start = json_tag_value_start(filedata, "image_size");
    if (start == NULL) {
        set_upload_message("No image_size");
        return -1;
    }
    *supplied_size = atoi(start);
    return *supplied_size != 0;
}

static int fc_find_board_id_in_json(unsigned int *supplied, const char *filedata)
{
    char *start = json_tag_value_start(filedata, "board_id");
    if (start == NULL) {
        set_upload_message("No board_id");
        return -1;
    }
    *supplied = atoi(start);
    return *supplied != 0;
}

void handle_ardupilot_upgrade_px4(struct template_state *tmpl, const char *filename, const char *filedata, uint32_t size)
{
    console_printf("ardupilot_upgrade: size=%u\n", size);
    if (serial_port_fd == -1) {
        set_upload_message("Not connected via serial port");
        return;
    }

    set_upload_progress(1);
    if (filedata[0] != '{') {
        fprintf(stderr, "no brace\n");
        abort();
    }

    char *start = json_tag_value_start(filedata, "image");
    start += 1; // skip opening quote
    if (start == NULL) {
        set_upload_message("Bad upload");
        return;
    }
    unsigned int supplied_image_size;
    if (fc_find_image_size_in_json(&supplied_image_size, filedata) != 0) {
        set_upload_message("No supplied image size");
    }
    unsigned int supplied_board_id;
    if (fc_find_board_id_in_json(&supplied_board_id, filedata) != 0) {
        set_upload_message("No supplied board type");
    }
    /* uint32_t fw_offset; */
    /* fw_offset = start-filedata; */
    /* fprintf(stderr, "firmware offset=%u (%x)\n", fw_offset, start[0]); */
    unsigned long total_length = 0;
    unsigned char supplied_md5[16];
    if (fc_find_md5_in_json(supplied_md5, filedata) == 0) {
        struct readstate my_state;
        init_mystate(&my_state);
        my_state.base64_src = start;

        unsigned char calculated_md5[16];
        fc_firmware_md5(my_read, (void*)&my_state, calculated_md5, &total_length);
        dump_md5("supplied", supplied_md5);
        dump_md5("calculated", calculated_md5);
        if (memcmp(calculated_md5, supplied_md5, sizeof(calculated_md5))) {
            set_upload_message("md5sum failed");
            return;
        }
        set_upload_message("md5sum match");
    } else {
        // still want the length of the binary for progress
        struct readstate my_state;
        init_mystate(&my_state);
        my_state.base64_src = start;
        fc_firmware_length(my_read, (void*)&my_state, &total_length);
    }

    if (supplied_image_size != 0 &&
        total_length != supplied_image_size) {
        fprintf(stderr, "image size mismatch (%lu) vs (%u)", total_length, supplied_image_size);
        set_upload_message("image size mismatch");
        return;
    }

    struct readstate my_state;
    init_mystate(&my_state);
    my_state.base64_src = start;

    upgrade_fc_firmware(my_read, (void*)&my_state, total_length, supplied_board_id);
}
