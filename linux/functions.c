#include "../includes.h"
#include "../template.h"
#include "functions.h"
#include "../functions.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/types.h>
#include <unistd.h>

void sock_send_ssid(struct sock_buf *sock, const char *ssid, const char *pass, const unsigned channel, const unsigned mode, const char *prefix)
{
    sock_printf(sock, "{ "
                "\"%sssid\": \"%s\", "
                "\"%spassword\" : \"%s\", "
                "\"%schannel\" : %d, "
                "\"%sauthmode\" : %d "
                "}",
                prefix,
                ssid,
                prefix,
                pass,
                prefix,
                channel,
                prefix,
                mode);
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
    /* .... and manlfunctions on TX1's Ubuntu 16.04 */
    snprintf(cmd, sizeof(cmd), "nmcli c s %s | grep \"%s:\" |  perl -pe 's/^[^ ]*[ ]*(.*)\n/$1/' >%s", ifname, setting_property, tmpfile);
    fprintf(stderr, "%s\n", cmd);
    if (system(cmd) == -1) {
        sock_printf(tmpl->sock, "system failed");
        return;
    }
    memset(ret, '\0', retlen);
    if (read(content_fd, ret, retlen) == -1) {
        sock_printf(tmpl->sock, "read failed");
        return;
    }
    close(content_fd);
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
    fprintf(stderr, "%s\n", cmd);
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


/*
 * work around nmcli variation on different versions of Ubuntu by
 * fetching secret directly from files.  Yay abusing root access.
 */
int nm_fetch_wpa_psk_secret(const char *ifname, char *password, const uint8_t passwordlen)
{
    char cmd[300];
    char tmpfile[] = "/tmp/nmcli.XXXXXX";
    int content_fd = mkstemp(tmpfile);
    if (content_fd == -1) {
        return -1;
    }
    /* TODO: do we REALLY want to link with the dbus libraries?! */
    /* note that the -s argument to nmcli is not present on Raspbian/Jessie */
    /* .... and manlfunctions on TX1's Ubuntu 16.04 */
    snprintf(cmd, sizeof(cmd), "cat /etc/NetworkManager/system-connections/%s | grep '^psk=' | cut -f 2 -d '=' | perl -pe 's/\n//'g >%s", ifname, tmpfile);
    fprintf(stderr, "%s\n", cmd);
    if (system(cmd) == -1) {
        return -1;
    }
    memset(password, '\0', passwordlen);
    if (read(content_fd, password, passwordlen) == -1) {
        return -1;
    }
    close(content_fd);
    return 0;
}

#define streq(a,b) (strcmp(a,b) == 0)

static const char *ifname = "WiFiAP";
static const char *ifname_prefix = "";

/*
  get ssid and password
 */
static const char *prop_ssid = "802-11-wireless.ssid";
static const char *prop_sectype = "802-11-wireless-security.key-mgmt";
static const char *prop_channel = "802-11-wireless.channel";
static const char *prop_key = "802-11-wireless-security.psk";
static const char *prop_wireless_mode = "802-11-wireless.mode";
static const char *prop_wireless_auth_alg = "802-11-wireless-security.auth-alg";

static void sock_send_ssid_for_ifname(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv, const char *ifname, const char *prefix);

static void get_ssid(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    sock_send_ssid_for_ifname(tmpl, name, value, argc, argv, ifname, ifname_prefix);
}

static void sock_send_ssid_for_ifname(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv, const char *ifname, const char *prefix)
{
    char ssid[50]="", pass[50], security_type[50]="";
    int mode=0, channel=0;

    nmcli_property_string_get(tmpl, ifname, prop_ssid, ssid, sizeof(ssid));
    nmcli_property_string_get(tmpl, ifname, prop_sectype, security_type, sizeof(security_type));
    nmcli_property_integer_get(tmpl, ifname, prop_channel, &channel);

    if (streq(security_type, "wpa-psk")) {
        // nmcli_property_string_get(tmpl, ifname, prop_key, pass, sizeof(pass));
        if (nm_fetch_wpa_psk_secret(ifname, pass, sizeof(pass)) == -1) {
            pass[0] = '\0';
        }
        mode = AUTH_WPA2;
    } else {
        mode = -1;
    }

    sock_send_ssid(tmpl->sock, ssid, pass, channel, mode, prefix);
}


/*
  set ssid and password
 */
static void set_ssid_for_ifname(struct template_state *tmpl,
                                const char *name,
                                const char *value,
                                int argc,
                                char **argv,
                                const char *ifname);

static void set_ssid(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    set_ssid_for_ifname(tmpl, name, value, argc, argv, ifname);
}

static const char *authmode_to_key_mgmt_string(int mode)
{
    switch (mode) {
    case AUTH_WPA:
    case AUTH_WPA2:
        return "wpa-psk";
    }
    return NULL;
}


static void set_ssid_for_ifname(struct template_state *tmpl,
                                const char *name,
                                const char *value,
                                int argc,
                                char **argv,
                                const char *ifname) {
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
        return;
    }

    int auth_mode = atoi(authtype);
    int ap_channel = atoi(channel);

    const char *ret = validate_ssid_password(ssid, password, auth_mode, ap_channel);
    if (ret) {
        // invalid input
        sock_printf(tmpl->sock, "%s", ret);
        return;
    }

    console_printf("IF=%s SSID='%s' password='%s' authtype=%s channel=%u\n",
                   ifname,
                   ssid,
                   password,
                   authtype,
                   ap_channel);

    const char *key_mgt = authmode_to_key_mgmt_string(auth_mode);
    if (key_mgt == NULL) {
        sock_printf(tmpl->sock, "WPA2 only");
        return;
    }
    nmcli_property_string_set(tmpl, ifname, prop_sectype, key_mgt);
    nmcli_property_integer_set(tmpl, ifname, prop_channel, ap_channel);
    nmcli_property_string_set(tmpl, ifname, prop_ssid, ssid);
    nmcli_property_string_set(tmpl, ifname, prop_key, password);

    sock_printf(tmpl->sock, "Set SSID and password");
}

static bool nmcli_interface_exists(struct template_state *tmpl,
                                   const char *ifname)
{
    char ssid[50];
    nmcli_property_string_get(tmpl, ifname, prop_ssid, ssid, sizeof(ssid));
    fprintf(stderr, "client ssid is %s\n", ssid);
    return strlen(ssid) != 0;
}

static bool nmcli_interface_create_client(struct template_state *tmpl,
                                          const char *ifname,
                                          const char *ssid)
{
    char cmd[300];

    /* TODO: do we REALLY want to link with the dbus libraries?! */

    snprintf(cmd, sizeof(cmd), "nmcli connection add con-name %s type wifi ifname wlan0 ssid %s", ifname, ssid);
    fprintf(stderr, "%s\n", cmd);
    if (system(cmd) == -1) {
        return false;
    }

    nmcli_property_string_set(tmpl, ifname, "connection.autoreconnect", "yes");
    nmcli_property_string_set(tmpl, ifname, prop_wireless_mode, "infrastructure");
    nmcli_property_string_set(tmpl, ifname, prop_wireless_auth_alg, "open");
    return true;
}

static const char *client_ifname = "WiFiClient";
static const char *client_ifname_prefix = "client_";

static void get_client_ssid(struct template_state *tmpl, const char *name, const char *value, int argc, char **argv)
{
    if (!nmcli_interface_exists(tmpl, client_ifname)) {
        sock_send_ssid(tmpl->sock, "", "", 0, AUTH_WPA2, client_ifname_prefix);
        return;
    }

    sock_send_ssid_for_ifname(tmpl, name, value, argc, argv, client_ifname, client_ifname_prefix);
}


/*
  set ssid and password
 */
static void set_client_ssid(struct template_state *tmpl,
                            const char *name,
                            const char *value,
                            int argc,
                            char **argv)
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
        return;
    }

    int auth_mode = atoi(authtype);
    int ap_channel = atoi(channel);

    const char *ret = validate_ssid_password(ssid, password, auth_mode, ap_channel);
    if (ret) {
        // invalid input
        sock_printf(tmpl->sock, "%s", ret);
        return;
    }

    if (!nmcli_interface_exists(tmpl, client_ifname)) {
        if (!nmcli_interface_create_client(tmpl, client_ifname, ssid)) {
            sock_printf(tmpl->sock, "Client create failed");
            return;
        }
    }
    set_ssid_for_ifname(tmpl, name, value, argc, argv, client_ifname);
}

void linux_functions_init(struct template_state *tmpl)
{
    tmpl->put(tmpl, "get_ssid", "", get_ssid);
    tmpl->put(tmpl, "set_ssid", "", set_ssid);
    tmpl->put(tmpl, "get_client_ssid", "", get_client_ssid);
    tmpl->put(tmpl, "set_client_ssid", "", set_client_ssid);
}
