#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/printk.h>

#define HTTP_RX_BUFFER_SIZE 1024
#define WIFI_CONNECT_RETRIES 5

static K_SEM_DEFINE(ipv4_ready, 0, 1);
static K_SEM_DEFINE(wifi_connected, 0, 1);
static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                               struct net_if *iface)
{
    const struct wifi_status *status = cb->info;

    ARG_UNUSED(iface);

    if (event == NET_EVENT_WIFI_CONNECT_RESULT) {
        if (status->status == 0) {
            printk("WiFi asociado a %s\n", CONFIG_STA_SAMPLE_SSID);
            k_sem_give(&wifi_connected);
        } else {
            printk("WiFi fallo: %d\n", status->status);
        }
    } else if (event == NET_EVENT_WIFI_DISCONNECT_RESULT) {
        printk("WiFi desconectado\n");
    }
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb, uint64_t event,
                               struct net_if *iface)
{
    char address[NET_IPV4_ADDR_LEN];

    ARG_UNUSED(cb);

    if (event != NET_EVENT_IPV4_ADDR_ADD || iface == NULL) {
        return;
    }

    for (int index = 0; index < NET_IF_MAX_IPV4_ADDR; index++) {
        struct net_if_ipv4 *ipv4 = iface->config.ip.ipv4;

        if (ipv4->unicast[index].ipv4.addr_type != NET_ADDR_DHCP) {
            continue;
        }

        printk("IPv4: %s\n", net_addr_ntop(AF_INET,
            &ipv4->unicast[index].ipv4.address.in_addr,
            address, sizeof(address)));
        k_sem_give(&ipv4_ready);
        return;
    }
}

static int wifi_connect(void)
{
    struct net_if *iface = net_if_get_first_wifi();
    struct wifi_connect_req_params params = { 0 };

    if (iface == NULL) {
        printk("No se encontro interfaz WiFi\n");
        return -ENODEV;
    }

    params.ssid = (const uint8_t *)CONFIG_STA_SAMPLE_SSID;
    params.ssid_length = strlen(CONFIG_STA_SAMPLE_SSID);
    params.psk = (const uint8_t *)CONFIG_STA_SAMPLE_PASSWORD;
    params.psk_length = strlen(CONFIG_STA_SAMPLE_PASSWORD);
    params.security = WIFI_SECURITY_TYPE_PSK;
    params.channel = WIFI_CHANNEL_ANY;
    params.band = WIFI_FREQ_BAND_2_4_GHZ;
    params.mfp = WIFI_MFP_OPTIONAL;

    printk("Conectando WiFi a %s...\n", CONFIG_STA_SAMPLE_SSID);
    return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}

static int http_get(void)
{
    char request[256];
    char response[HTTP_RX_BUFFER_SIZE + 1];
    struct sockaddr_in server = { 0 };
    int socket_fd;
    int received;
    int request_length;
    int result;

    socket_fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        printk("HTTP socket fallo: %d\n", errno);
        return -errno;
    }

    server.sin_family = AF_INET;
    server.sin_port = htons(CONFIG_LAB_HTTP_SERVER_PORT);
    result = zsock_inet_pton(AF_INET, CONFIG_LAB_HTTP_SERVER_ADDR, &server.sin_addr);
    if (result != 1) {
        printk("Direccion HTTP invalida: %s\n", CONFIG_LAB_HTTP_SERVER_ADDR);
        zsock_close(socket_fd);
        return -EINVAL;
    }

    printk("Conectando HTTP a %s:%d...\n",
           CONFIG_LAB_HTTP_SERVER_ADDR, CONFIG_LAB_HTTP_SERVER_PORT);
    if (zsock_connect(socket_fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        printk("HTTP connect fallo: %d\n", errno);
        zsock_close(socket_fd);
        return -errno;
    }

    request_length = snprintk(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        CONFIG_LAB_HTTP_PATH, CONFIG_LAB_HTTP_SERVER_ADDR);
    if (zsock_send(socket_fd, request, request_length, 0) < 0) {
        printk("HTTP GET fallo: %d\n", errno);
        zsock_close(socket_fd);
        return -errno;
    }

    received = zsock_recv(socket_fd, response, sizeof(response) - 1, 0);
    if (received < 0) {
        printk("HTTP recv fallo: %d\n", errno);
        zsock_close(socket_fd);
        return -errno;
    }

    response[received] = '\0';
    printk("HTTP respuesta (%d bytes):\n%s\n", received, response);
    if (strncmp(response, "HTTP/1.1 200", 12) != 0 &&
        strncmp(response, "HTTP/1.0 200", 12) != 0) {
        printk("HTTP resultado distinto de 200 OK\n");
        zsock_close(socket_fd);
        return -EPROTO;
    }

    printk("HTTP GET completado: 200 OK\n");
    zsock_close(socket_fd);
    return 0;
}

int main(void)
{
    printk("\n\n===================================\n   ZEPHYR ARRANCO CORRECTAMENTE   \n===================================\n\n");

    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler,
                                 NET_EVENT_WIFI_CONNECT_RESULT |
                                 NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_cb);

    net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&ipv4_cb);

    for (int attempt = 1; attempt <= WIFI_CONNECT_RETRIES; attempt++) {
        if (wifi_connect() == 0 &&
            k_sem_take(&wifi_connected, K_SECONDS(10)) == 0) {
            break;
        }

        printk("Reintento WiFi %d/%d\n", attempt, WIFI_CONNECT_RETRIES);
        if (attempt == WIFI_CONNECT_RETRIES) {
            printk("La solicitud de conexion WiFi fallo\n");
            return -EIO;
        }

        k_sleep(K_SECONDS(2));
    }

    k_sem_take(&ipv4_ready, K_FOREVER);
    if (http_get() != 0) {
        printk("La peticion HTTP fallo\n");
        return -EIO;
    }

    return 0;
}
