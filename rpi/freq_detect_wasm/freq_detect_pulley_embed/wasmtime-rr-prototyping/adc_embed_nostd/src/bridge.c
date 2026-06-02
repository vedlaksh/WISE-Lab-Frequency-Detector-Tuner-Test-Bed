#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <zephyr/drivers/uart.h>
#include <math.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/wifi_mgmt.h>
// #include <zephyr/net/wifi_credentials.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_if.h>



static struct net_mgmt_event_callback wifi_cb;
static struct net_mgmt_event_callback ipv4_cb;

#define WIFI_EVENTS (NET_EVENT_WIFI_CONNECT_RESULT | \
                     NET_EVENT_WIFI_DISCONNECT_RESULT)

#define IPV4_EVENTS (NET_EVENT_IPV4_DHCP_BOUND | \
                     NET_EVENT_IPV4_ADDR_ADD)

#define WIFI_SSID "testwifi"
#define WIFI_PASS "testing123"

static const struct adc_dt_spec adc_chan =
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0);

static const struct device *rr_uart = 
    DEVICE_DT_GET(DT_CHOSEN(rr_uart));

int16_t sample;

struct adc_sequence sequence = {
    .buffer = &sample,
    .buffer_size = sizeof(sample),
};

// #define DEST_IP "172.26.4.255"
#define DEST_IP "192.168.137.1"
#define DEST_PORT 9000


static int sock = -1;
static struct sockaddr_in addr = {0};

static K_SEM_DEFINE(wifi_connected, 0, 1);
static K_SEM_DEFINE(ipv4_ready, 0, 1);

static volatile bool wifi_ok = false;
static volatile bool ipv4_ok = false;


void host_rr_write(const uint8_t *ptr, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(rr_uart, ptr[i]);
    }
}

// void receiver_thread(void *p1, void *p2, void *p3)
// {
//     ARG_UNUSED(p1);
//     ARG_UNUSED(p2);
//     ARG_UNUSED(p3);

//     // printk("receiver_thread ready!\n");
    
//     int recv_sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
//     printk("receiver: after socket sock=%d errno=%d\n", recv_sock, errno);

//     struct sockaddr_in recv_addr = {0};
//     recv_addr.sin_family = AF_INET;
//     recv_addr.sin_port = htons(9000);
//     recv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

//     int ret = zsock_bind(recv_sock, (struct sockaddr *)&recv_addr, sizeof(recv_addr));
//     printk("receiver: after bind ret=%d errno=%d\n", ret, errno);

//     uint8_t buf[64];

//     while (1) {
//         printk("receiver alive\n");
//         int len = zsock_recv(recv_sock, buf, sizeof(buf), 0);
//         if (len > 0) {
//             // printk("Received %d bytes: ", len);
//             // for (int i = 0; i < len; i++) {
//             //     printk("%02x ", buf[i]);
//             // }
//             // printk("\n");
//             host_rr_write(buf, (size_t)len);
//         }
        
//         k_sleep(K_SECONDS(1));
//     }
// }

// K_THREAD_DEFINE(rx_tid, 4096, receiver_thread, NULL, NULL, NULL, -1, 0, 0);

static void print_ipv4_addr(void)
{
    struct net_if *iface = net_if_get_default();

    if (!iface || !iface->config.ip.ipv4) {
        printk("No IPv4 config yet\n");
        return;
    }

    for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
        if (iface->config.ip.ipv4->unicast[i].ipv4.is_added) {
            char buf[NET_IPV4_ADDR_LEN];

            net_addr_ntop(AF_INET,
                          &iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr,
                          buf, sizeof(buf));

            printk("ESP32 IP: %s\n", buf);
        }
    }
}

static void wifi_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event,
                               struct net_if *iface)
{

    printk("wifi cb event=0x%llx\n", mgmt_event);

    switch (mgmt_event) {
    case NET_EVENT_WIFI_CONNECT_RESULT: {
        const struct wifi_status *status =
            (const struct wifi_status *)cb->info;
        int st = status ? status->status : -999;
        printk("connect result status=%d\n", st);
        wifi_ok = (st == 0);
        k_sem_give(&wifi_connected);
        break;
    }
    case NET_EVENT_WIFI_DISCONNECT_RESULT:
        printk("disconnect result\n");
        break;
    default:
        break;
    }
}

static void ipv4_event_handler(struct net_mgmt_event_callback *cb,
                               uint64_t mgmt_event,
                               struct net_if *iface)
{

    printk("ipv4 cb event=0x%llx\n", mgmt_event);

    switch (mgmt_event) {
    case NET_EVENT_IPV4_DHCP_BOUND:
        printk("dhcp bound\n");
        ipv4_ok = true;
        k_sem_give(&ipv4_ready);
        break;
    case NET_EVENT_IPV4_ADDR_ADD:
        printk("ipv4 addr add\n");
        break;
    default:
        break;
    }
}

static int wifi_connect_blocking(void)
{
    printk("APP wifi_connect_blocking entered\n");
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        printk("No default net_if\n");
        return -1;
    }

    wifi_ok = false;
    ipv4_ok = false;
    k_sem_reset(&wifi_connected);
    k_sem_reset(&ipv4_ready);

    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, WIFI_EVENTS);
    net_mgmt_add_event_callback(&wifi_cb);

    net_mgmt_init_event_callback(&ipv4_cb, ipv4_event_handler, IPV4_EVENTS);
    net_mgmt_add_event_callback(&ipv4_cb);

    struct wifi_connect_req_params params = {
        .ssid = WIFI_SSID,
        .ssid_length = strlen(WIFI_SSID),
        .psk = WIFI_PASS,
        .psk_length = strlen(WIFI_PASS),
        .security = WIFI_SECURITY_TYPE_PSK,
        .channel = WIFI_CHANNEL_ANY,
        .mfp = WIFI_MFP_OPTIONAL,
        .timeout = SYS_FOREVER_MS,
    };

    int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    printk("connect request ret=%d\n", ret);
    if (ret != 0) {
        printk("connect request failed!\n");
        return ret;
    }

    ret = k_sem_take(&ipv4_ready, K_SECONDS(5));
    if (ret == 0) {
        printk("got ipv4_ready\n");
        print_ipv4_addr();
        return 0;
    }

    printk("Timed out waiting for DHCP/IPv4\n");
    return -ETIMEDOUT;

    // int x = 5 / 0;

    out:
        net_mgmt_del_event_callback(&wifi_cb);
        net_mgmt_del_event_callback(&ipv4_cb);
        return ret;
    // return 0;
}

int wifi_init_and_connect(void)
{
    printk("APP wifi_init_and_connect entered\n");
    int ret = wifi_connect_blocking();
    if (ret != 0) {
        printk("wifi_connect_blocking failed: %d\n", ret);
        return ret;
    }

    sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    printk("sender: socket=%d errno=%d\n", sock, errno);
    if (sock < 0) {
        printf("socket Initialization Error\n");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEST_PORT);

    ret = zsock_inet_pton(AF_INET, DEST_IP, &addr.sin_addr);
    printk("sender: inet_pton ret=%d errno=%d\n", ret, errno);
    if (ret != 1) {
        zsock_close(sock);
        sock = -1;
        return -1;
    }

    return 0;
}

int send_bytes(const char *ptr, size_t len)
{
    if (sock < 0) {
        printk("send_bytes: socket not initialized\n");
        return -1;
    }

    char ipbuf[NET_IPV4_ADDR_LEN];
    zsock_inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    printk("sending to %s:%d\n", ipbuf, ntohs(addr.sin_port));

    int ret = zsock_sendto(sock, ptr, len, 0,
                           (struct sockaddr *)&addr, sizeof(addr));

    printk("sendto ret=%d errno=%d\n", ret, errno);
    return ret;
}

// void wifi_connect_blocking(void)
// {
//     struct net_if *iface = net_if_get_default();

//     net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, WIFI_EVENT_MASK);
//     net_mgmt_add_event_callback(&wifi_cb);

//     struct wifi_connect_req_params params = {
//         .ssid = WIFI_SSID,
//         .ssid_length = strlen(WIFI_SSID),
//         .psk = WIFI_PASS,
//         .psk_length = strlen(WIFI_PASS),
//         .security = WIFI_SECURITY_TYPE_PSK,
//         .channel = WIFI_CHANNEL_ANY,
//         .mfp = WIFI_MFP_OPTIONAL,
//         .timeout = SYS_FOREVER_MS,
//     };

//     int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface,
//                        &params, sizeof(params));
//     printk("connect request ret=%d\n", ret);

//     k_sem_take(&wifi_connected, K_FOREVER);

//     /* DHCP may still need a moment */
//     k_sleep(K_SECONDS(3));
//     print_ipv4_addr();
// }

// void wifi_init_and_connect(){
//     wifi_connect_blocking();
//     sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
//     printk("sender: after socket sock=%d errno=%d\n", sock, errno);
//     addr.sin_family = AF_INET;
//     addr.sin_port = htons(DEST_PORT);
//     int ret = zsock_inet_pton(AF_INET, DEST_IP, &addr.sin_addr);
//     printk("sender: after socket sock=%d errno=%d\n", ret, errno);
// }

int host_rr_uart_init(void) {
    if (!device_is_ready(rr_uart)) {
        printk("RR UART not ready\n");
        return -19;
    }
    printk("RR UART ready and modified!\n");
    // const char *test = "RR UART OK\r\n";
    // for (int i = 0; test[i]; i++) {
    //     uart_poll_out(rr_uart, test[i]);
    // }
    return 0;
}

void host_printk(const char *ptr, size_t len){
    printk("%.*s", (int)len, ptr);
    return;
}

int host_adc_init(){
    printk("host_adc_init entered!\n");
    int err;
    err = adc_channel_setup_dt(&adc_chan);
	if (err) {
		printk("adc_channel_setup_dt failed: %d\n", err);
		return err;
	}

	err = adc_sequence_init_dt(&adc_chan, &sequence);
	if (err) {
		printk("adc_sequence_init_dt failed: %d\n", err);
		return err;
	}

    host_rr_uart_init();

    wifi_init_and_connect();

    return 0;
}

int host_adc_read(){
    int err;
    err = adc_read_dt(&adc_chan, &sequence);
    if (err) {
        printk("adc_read_dt failed: %d\n", err);
        return err;
    }
    return (int)sample;
}

int host_adc_raw_to_millivolts(int32_t avg_mv){
    adc_raw_to_millivolts_dt(&adc_chan, &avg_mv);
    return avg_mv;
}

// void host_k_busy_wait(int us) {
//     k_busy_wait(us);
//     return;
// }

// int host_dev_msleep(int ms) {
//     int32_t conv_ms = (int32_t)ms;
//     int32_t conv_ret = (int32_t)k_msleep(conv_ms);
//     return conv_ret;
// }


double host_sin(double x) { return sin(x); }
double host_cos(double x) { return cos(x); }
// uint32_t host_get_time_ms(void) { return (uint32_t)k_uptime_get(); }



