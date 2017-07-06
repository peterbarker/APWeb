#include "helper_functions.h"

#include "bootloader_client.h"

#include "../web_server.h" // FIXME!

#ifdef _POSIX_VERSION
#include "../posix/functions.h"
#include "../posix/helper_functions.h"
#endif

#include <stdio.h>
#include <termios.h>

ssize_t bootloader_write(const uint8_t *bytes, const size_t bytecount, const bl_timeout_t timeout)
{
    const ssize_t written = write(serial_port_fd, bytes, bytecount);
    tcflush(serial_port_fd, TCIOFLUSH);
    if (written < bytecount) {
        abort();
    }
    return written;
}

static void debug(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    fprintf(stderr, "%lu.%06lu: ", tv.tv_sec, tv.tv_usec);
    vfprintf(stderr, format, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

ssize_t bootloader_readbyte(uint8_t *byte, const bl_timeout_t timeout)
{
    ssize_t numread = 0;
    fd_set fds, fd_errs;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = timeout*1000;

    FD_ZERO(&fds);
    FD_SET(serial_port_fd, &fds);
    FD_SET(serial_port_fd, &fd_errs);

    int selret = select(serial_port_fd+1, &fds, NULL, &fd_errs, &tv);
    if (selret == -1) {
        debug("select error");
        return 0;
    }
    if (selret == 0) {
        debug("select timeout");
        return 0;
    }
    if (!FD_ISSET(serial_port_fd, &fds)) {
        debug("not set");
        return 0;
    }
    if (FD_ISSET(serial_port_fd, &fd_errs)) {
        debug("error");
        return 0;
    }
    numread += read(serial_port_fd, byte, 1);
    debug("got byte (0x%02x)", *byte);
    return numread;
}

void bootloader_debug(const char  *format, ...)
{
    va_list ap;
    char msgbuf[100];

    va_start(ap, format);
    vsnprintf(msgbuf,sizeof(msgbuf), format, ap);
    msgbuf[sizeof(msgbuf)-1] = '\0';
    set_upload_message(msgbuf);
    vfprintf(stderr, format, ap);
    va_end(ap);
}

static void failmessage(const char *msg)
{
    debug("%s", msg);
    set_upload_message(msg);
}

 void drain_serial()
{
    tcflush(serial_port_fd, TCIFLUSH);
    tcflush(serial_port_fd, TCOFLUSH);
    uint8_t unused;
    while (bootloader_readbyte(&unused, 0) != 0) {
        debug("drained a byte");
    }
    debug("done draining");
}


typedef struct {
    uint32_t baud;
    const char *name;
} serial_config_t;
const serial_config_t serial_configs[] = {
    { B115200, "115200" },
    { B921600, "921600" },
    { B38400, "38400" },
    { B57600, "57600" },
};
#define ARRAY_LEN(x) (sizeof(x)/sizeof(x[0]))
static int try_get_sync()
{
    struct termios options;
    if (tcgetattr(serial_port_fd, &options) == -1) {
        failmessage("Failed to get serial options: %m\n");
        return -1;
    }
    // attempt to sync at all rates:
    for (uint8_t i=0; i<ARRAY_LEN(serial_configs); i++) {
        serial_config_t config = serial_configs[i];
        set_upload_message("Resetting baud rate");
        debug("Trying baud: %s", config.name);
        cfsetispeed(&options, config.baud);
        cfsetospeed(&options, config.baud);
        if (tcsetattr(serial_port_fd, TCSANOW, &options) == -1) {
            debug("Failed to set serial (%i) to %s: %m", serial_port_fd, config.name);
            continue;
        }
        for (uint8_t j=0;j<5;j++) {
            drain_serial();
            if (bootloader_get_sync() == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int get_bootloader()
{
    struct termios options;
    if (tcgetattr(serial_port_fd, &options) == -1) {
        failmessage("Failed to get serial options: %m\n");
        return -1;
    }

    // try syncing without mavlink reboot first - avoid confusing the
    // bootloader
    int gs_ret = try_get_sync();
    if (gs_ret == 1) {
        return 0;
    }
    if (gs_ret == -1) {
        return -1;
    }

    for (uint8_t k=0; k<5; k++) { // 5 retries
        bootloader_send_mavlink_reboot();
        // attempt to reboot at all rates:
        for (uint8_t i=0; i<ARRAY_LEN(serial_configs); i++) {
            serial_config_t config = serial_configs[i];
            cfsetispeed(&options, config.baud);
            cfsetospeed(&options, config.baud);
            if (tcsetattr(serial_port_fd, TCSANOW, &options) == -1) {
                debug("Failed to set serial (%i) to %s for mavlink reboot: %m", serial_port_fd, config.name);
                continue;
            }
            bootloader_send_mavlink_reboot();
            usleep(100000);
        }
        int gs_ret = try_get_sync();
        if (gs_ret == 1) {
            return 0;
        }
        if (gs_ret == -1) {
            return -1;
        }
    }
    return -1;
}

void upgrade_fc_firmware(int(read)(void*,char*,int), void *readstate, const unsigned long total_length, const uint32_t expected_board_id)
{
    char buffer[bl_prog_multi_chunksize];
    unsigned long total_written = 0;

    debug("upgrade_fc_firmware called");
    lock_fc_serial();

    struct termios options; // so we can restore
    if (tcgetattr(serial_port_fd, &options) == -1) {
        failmessage("Failed to get serial options: %m\n");
        goto OUT_BAD_GETATTR;
    }

    if (get_bootloader() == -1) {
        failmessage("Failed to get bootloader");
        goto OUT_BAD_BOOTLOADER;
    }


    uint32_t fc_board_id;
    if (bootloader_get_board_id(&fc_board_id) == -1) {
        set_upload_message("Failed to retrieve board ID");
        goto OUT_BAD_BOOTLOADER;
    }

    if (fc_board_id != expected_board_id) {
        set_upload_message("This firmware is not appropriate for this board");
        goto OUT_BAD_BOOTLOADER;
    }

    set_upload_message("Board ID correct");

    set_upload_message("Erasing Flash");
    bootloader_erase_send();
    const int steps = 30; // time to wait in seconds
    int erase_done = 0;
    for (uint8_t i=0; i<steps; i++) {
        bl_reply_t status;
        if (bootloader_recv_sync(&status, 100) == -1) {
            debug("Failed to get sync");
        } else {
            debug("Got sync");
            erase_done = 1;
            break;
        }
        set_upload_progress(100*i/steps);
        usleep(1000000);
    }
    if (!erase_done) {
        failmessage("Timeout waiting for erase to complete");
        goto OUT_BAD_BOOTLOADER;
    }

    set_upload_progress(1);
    uint32_t calculated_crc = 0;
    while (1) {
        struct timeval tv_start;
        struct timeval tv_stop;
        gettimeofday(&tv_start, NULL);
        int num_read = read(readstate, buffer, sizeof(buffer));
        gettimeofday(&tv_stop, NULL);
        if (num_read == 0) {
            // done - I hope
            debug("Done - I hope");
            break;
        }
        debug("read %u bytes", num_read);
        if (bootloader_program_chunk((unsigned char*)buffer, num_read) != 0) {
            debug("Error writing to bootloader");
            goto OUT_BAD_BOOTLOADER;
        }
        total_written += num_read;
        if (total_length != 0) {
            set_upload_progress(100*total_written/total_length);
        }

        // update our CRC:
        calculated_crc = bootloader_crc32_chunk((unsigned char*)buffer, num_read, calculated_crc);
    }

    // finalise calculation of our CRC:
    uint32_t fw_size;
    if (bootloader_get_fw_size(&fw_size) == -1) {
    	debug("Failed to get fw_size");
    	goto OUT_BAD_BOOTLOADER;
    }
    debug("Got fw_size (%u)", fw_size);

    uint32_t padlen = fw_size;
    const uint8_t bootloader_crcpad[] =  { '\xff', '\xff', '\xff', '\xff' };
    unsigned i=0;
    for (i=total_written; i<padlen; i+=4) {
        calculated_crc = bootloader_crc32_chunk(bootloader_crcpad, 4, calculated_crc);
    }

    uint32_t fc_crc;
    if (bootloader_get_crc(&fc_crc) == -1) {
        set_upload_message("Failed to get FC CRC");
        goto OUT_BAD_BOOTLOADER;
    } else {
        debug("Got FC crc (%x)", fc_crc);
    }
    if (calculated_crc != fc_crc) {
        debug("CRC mismatch (calculated=%x) (bl says %x)", calculated_crc, fc_crc);
        set_upload_message("CRC mismatch");
        goto OUT_BAD_BOOTLOADER;
    }

    set_upload_message("CRCs match");
    debug("CRCs match");

    set_upload_message("Booting ArduPilot");
    if (bootloader_boot() == -1) {
        set_upload_message("Failed to boot ArduPilot");
        goto OUT_BAD_BOOTLOADER;
    }
    set_upload_message("Upload complete");

OUT_BAD_BOOTLOADER:
    tcsetattr(serial_port_fd, TCSANOW, &options);
OUT_BAD_GETATTR:
    unlock_fc_serial();
}

