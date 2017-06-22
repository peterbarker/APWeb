#include "upgrade_px4.h"

#ifdef _POSIX_VERSION
#include "posix/functions.h"
#include "posix/helper_functions.h"
#endif


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

struct readstate {
    const char *base64_src;
    unsigned offset;
};

static int my_read(void *_my_state, char *ret, const int len)
{
    fprintf(stderr, "my_read called for (%u) bytes\n", len);
    struct readstate *my_state = (struct readstate*)_my_state;
    int bytes_read = 0;
    while (bytes_read < len &&
           my_state->base64_src[my_state->offset] != '"') {
        ret[bytes_read] = my_state->base64_src[my_state->offset];
        my_state->offset++;
        bytes_read++;
    }
    return bytes_read;
}

void handle_ardupilot_upgrade_px4(struct template_state *tmpl, const char *filename, const char *filedata, uint32_t size)
{
    console_printf("ardupilot_upgrade: size=%u\n", size);
    set_upload_message("checking firmware MD5");
    set_upload_progress(1);
    uint32_t fw_offset;
    if (filedata[0] == '{') {
        fprintf(stderr, "brace\n");
        // I'm thoroughly convinced this is a JSON file upload...
        // decode the firmware in place:
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
        upgrade_fc_firmware(my_read, (void*)&my_state);
    } else {
#ifdef SYSTEM_FREERTOS
        // this is a .abin - right?
        if (!check_fc_fw_md5(__DECONST(uint8_t*,filedata), size, &fw_offset)) {
            set_upload_message("Bad MD5 on image");
            return;
        }
#else
        set_upload_message("Bad upload");
        return;
#endif
    }
    set_upload_message("Good MD5 on image - starting upgrade");

}
