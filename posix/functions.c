#include "../includes.h"
#include "../template.h"
#include "functions.h"
#include "../functions.h"

#include <sys/statvfs.h>

#include <sys/types.h>
#include <dirent.h>

static void sock_send_diskinfo(struct sock_buf *sock, const char *label, unsigned serial, unsigned total_clusters, unsigned free_clusters, unsigned cluster_size)
{
    sock_printf(sock,
                "{"
                "\"label\" : \"%s\", "
                "\"serial\" : %u, "
                "\"total_clusters\" : %u, "
                "\"free_clusters\" : %u, "
                "\"cluster_size\" : %u"
                "}",
                label, serial, total_clusters, free_clusters, cluster_size*512);
}

static void disk_info(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    unsigned long free_clusters = 0, total_clusters = 0, cluster_size = 0;
    const char label[] = "/";

    /* Get volume information and free clusters of root filesystem */
    struct statvfs buf;
    if (statvfs("/", &buf) != -1) {
        total_clusters = buf.f_blocks;
        free_clusters = buf.f_bfree;
        cluster_size = buf.f_bsize / 512;
    }

    const unsigned serial = 0;

    sock_send_diskinfo(tmpl->sock, label, serial, total_clusters, free_clusters, cluster_size);
}


static void sock_send_dirent(struct sock_buf *sock, bool first, int type, const char *name, unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute, unsigned second, unsigned size)
{
    if (!first) {
        sock_printf(sock, "%s", ",\n");
    }
    sock_printf(sock, "{"
                "\"type\" : %u, "
                "\"name\" : \"%s\", "
                "\"date\" : \"%04u-%02u-%02u %02u:%02u:%02u\", "
                "\"size\" : %u "
                "}",
                type, name, year, month, day, hour, minute, second, size);
}

static void file_listdir(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    if (argc < 1) {
        return;
    }
    const char *dirpath = argv[0];

    DIR *dh = opendir(dirpath);
    if (dh == NULL) {
        console_printf("Failed to open directory %s\n", dirpath);
        return;
    }

    sock_printf(tmpl->sock, "[ ");

    // from readdir(3):
    uint32_t name_max = pathconf(dirpath, _PC_NAME_MAX);
    if (name_max == -1) {        /* Limit not defined, or error */
        name_max = 255;         /* Take a guess */
    }
    uint32_t len = offsetof(struct dirent, d_name) + name_max + 1;
    struct dirent *entry = (struct dirent *)malloc(len);
    if (entry == NULL) {
        console_printf("Failed to allocate memory\n");
        return;
    }

    bool first = true;
    struct dirent *result;
    while ((readdir_r(dh, entry, &result)) == 0) {
        if (result == NULL) {
            break;
        }
        if (strcmp(result->d_name, ".") == 0) {
            continue;
        }

        struct stat buf;
        char filepath[PATH_MAX] = {};
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, result->d_name);
        if (stat(filepath, &buf) == -1) {
            continue;
        }

        struct tm t;
        if (localtime_r(&buf.st_mtim.tv_sec, &t) == NULL) {
            continue;
        }

        sock_send_dirent(tmpl->sock,
                         first,
                         (result->d_type==DT_DIR) ? 1 :0,
                         result->d_name,
                         t.tm_year+1900,
                         t.tm_mon+1,
                         t.tm_mday,
                         t.tm_hour,
                         t.tm_min,
                         t.tm_sec,
                         buf.st_size);
        first = false;
    }
    sock_printf(tmpl->sock, "]");
    closedir(dh);
}

static void nmcli_property_string_get(struct template_state *tmpl, const char *ifname, const char *setting_property, char *ret, unsigned retlen)
{
    char cmd[300];
    char tmpfile[] = "/tmp/nmcli.XXXXXX";
    int content_fd = mkstemp(tmpfile);
    if (content_fd == -1) {
        sock_printf(tmpl->sock, "mkstemp failed");
        return;
    }
    /* TODO: do we REALLY want to link with the dbus libraries?! */
    /* note that the -s argument to nmcli is not present on Raspbian/Jessie */
    snprintf(cmd, sizeof(cmd), "nmcli c s %s | grep \"%s:\" |  perl -pe 's/^[^ ]*[ ]*(.*)\n/$1/' >%s", ifname, setting_property, tmpfile);
    if (system(cmd) == -1) {
        sock_printf(tmpl->sock, "system failed");
        return;
    }
    if (read(content_fd, ret, retlen) == -1) {
        sock_printf(tmpl->sock, "read failed");
        return;
    }
}

static void nmcli_property_integer_get(struct template_state *tmpl, const char *ifname, const char *setting_property, int *ret)
{
    char text[300];
    nmcli_property_string_get(tmpl, ifname, setting_property, text, sizeof(text));
    *ret = atoi(text);
}


static void nmcli_property_string_set(struct template_state *tmpl, const char *ifname, const char *setting_property, const char *value)
{
    char cmd[300];
    /* TODO: do we REALLY want to link with the dbus libraries?! */
    snprintf(cmd, sizeof(cmd), "nmcli c m %s %s %s", ifname, setting_property, value);
    if (system(cmd) == -1) {
        sock_printf(tmpl->sock, "system failed");
        return;
    }
}

static void nmcli_property_integer_set(struct template_state *tmpl, const char *ifname, const char *setting_property, int value)
{
    char text[300];
    snprintf(text, sizeof(text), "%d", value);
    nmcli_property_string_set(tmpl, ifname, setting_property, text);
}

void sock_send_ssid(struct sock_buf *sock, const char *ssid, const char *pass, const unsigned channel, const unsigned mode)
{
    sock_printf(sock, "{ "
                "\"ssid\": \"%s\", "
                "\"password\" : \"%s\", "
                "\"channel\" : %d, "
                "\"authmode\" : %d "
                "}",
                ssid, pass, channel, mode);
}

#include "../functions.h" // for apweb_enc_auth_t

#define streq(a,b) (strcmp(a,b) == 0)

static const char *ifname = "wlan0";

/*
  get ssid and password
 */
static const char *prop_ssid = "802-11-wireless.ssid";
static const char *prop_sectype = "802-11-wireless-security.key-mgmt";
static const char *prop_channel = "802-11-wireless.channel";
static const char *prop_key = "802-11-wireless-security.psk";
static void get_ssid(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    char ssid[50]="", pass[50], security_type[50]="";
    int mode=0, channel=0;

    nmcli_property_string_get(tmpl, ifname, prop_ssid, ssid, sizeof(ssid));
    nmcli_property_string_get(tmpl, ifname, prop_sectype, security_type, sizeof(security_type));
    nmcli_property_integer_get(tmpl, ifname, prop_channel, &channel);

    if (streq(security_type, "wpa-psk")) {
        nmcli_property_string_get(tmpl, ifname, prop_key, pass, sizeof(pass));
        mode = AUTH_WPA2;
    } else {
        mode = -1;
    }

    sock_send_ssid(tmpl->sock, ssid, pass, channel, mode);
}


/*
  set ssid and password
 */
static void set_ssid(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    if (argc < 4) {
        sock_printf(tmpl->sock, "invalid call\n");
        return;
    }
    const char *ssid = argv[0];
    const char *password = argv[1];
    const char *authtype = argv[2];
    const char *channel = argv[3];
    if (!ssid || !password || !authtype || !channel) {
        sock_printf(tmpl->sock, "invalid SSID or password\n");
    }

    int auth_mode = atoi(authtype);
    int ap_channel = atoi(channel);

    const char *ret = validate_ssid_password(ssid, password, auth_mode, ap_channel);
    if (ret) {
        // invalid input
        sock_printf(tmpl->sock, "%s", ret);
        return;
    }

    console_printf("Setting SSID='%s' password='%s' authtype=%s channel=%u\n", ssid, password, authtype, ap_channel);

    nmcli_property_integer_set(tmpl, ifname, prop_sectype, auth_mode);
    nmcli_property_integer_set(tmpl, ifname, prop_channel, ap_channel);
    nmcli_property_string_set(tmpl, ifname, prop_ssid, ssid);
    nmcli_property_string_set(tmpl, ifname, prop_key, password);

    sock_printf(tmpl->sock, "Set SSID and password");
}




void posix_functions_init(struct template_state *tmpl)
{
    tmpl->put(tmpl, "file_listdir", "", file_listdir);
    tmpl->put(tmpl, "disk_info", "", disk_info);
    tmpl->put(tmpl, "get_ssid", "", get_ssid);
    tmpl->put(tmpl, "set_ssid", "", set_ssid);
}
