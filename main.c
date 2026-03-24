#include <libwebsockets.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>

/* -------------------- 数据结构 -------------------- */
struct ws_client {
    struct lws *wsi;
    struct ws_client *next;
};
static struct ws_client *clients = NULL;  /* WebSocket 客户端链表 */

/* 用于存储串口上下文（可选，便于在 WebSocket 回调中获取串口 wsi） */
static struct lws *serial_wsi = NULL;

/* -------------------- 串口配置 -------------------- */
int open_serial(const char *device, int baudrate) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        lwsl_err("Failed to open %s\n", device);
        return -1;
    }

    struct termios options;
    tcgetattr(fd, &options);
    cfsetispeed(&options, baudrate);
    cfsetospeed(&options, baudrate);

    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~PARENB;  /* 无校验 */
    options.c_cflag &= ~CSTOPB;  /* 1 位停止位 */
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;      /* 8 位数据 */

    /* 原始模式，不进行特殊处理 */
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_iflag &= ~(IXON | IXOFF | IXANY); /* 禁用流控 */
    options.c_oflag &= ~OPOST;

    tcsetattr(fd, TCSANOW, &options);
    return fd;
}

/* -------------------- 广播数据给所有 WebSocket 客户端 -------------------- */
static void broadcast_to_websockets(const unsigned char *data, size_t len) {
    struct ws_client *client, *next;
    unsigned char *buf;

    /* 遍历链表，注意：写入时可能删除节点，需安全迭代 */
    for (client = clients; client; client = next) {
        next = client->next;
        /* 为每个消息预留 LWS_PRE 字节 */
        buf = malloc(LWS_PRE + len);
        if (!buf) {
            lwsl_err("Out of memory\n");
            continue;
        }
        memcpy(buf + LWS_PRE, data, len);
        int n = lws_write(client->wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
        free(buf);
        if (n < 0) {
            /* 写入失败，关闭连接 */
            lwsl_err("Write failed, closing client\n");
            lws_close_reason(client->wsi, LWS_CLOSE_STATUS_GOINGAWAY,
                             (unsigned char *)"write error", 11);
        }
    }
}

/* -------------------- WebSocket 协议回调 -------------------- */
static int callback_ws(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len) {
    struct ws_client *client, *prev;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED:
            client = malloc(sizeof(struct ws_client));
            if (!client) return -1;
            client->wsi = wsi;
            client->next = clients;
            clients = client;
            lwsl_notice("WebSocket client connected\n");
            break;

        case LWS_CALLBACK_RECEIVE:
            /* 收到来自浏览器的数据（可选：转发到串口） */
            if (serial_wsi) {
                /* 注意：RAW 连接使用 lws_write 写入，需要自己管理缓冲区 */
                lws_write(serial_wsi, (unsigned char *)in, len, LWS_WRITE_RAW);
            }
            break;

        case LWS_CALLBACK_CLOSED:
            /* 从链表中移除该客户端 */
            prev = NULL;
            for (client = clients; client; client = client->next) {
                if (client->wsi == wsi) {
                    if (prev) prev->next = client->next;
                    else clients = client->next;
                    free(client);
                    break;
                }
                prev = client;
            }
            lwsl_notice("WebSocket client disconnected\n");
            break;

        default:
            break;
    }
    return 0;
}

/* -------------------- RAW 串口协议回调 -------------------- */
static int callback_raw_serial(struct lws *wsi, enum lws_callback_reasons reason,
                               void *user, void *in, size_t len) {
    switch (reason) {
        case LWS_CALLBACK_RAW_ADOPT_FILE:
            lwsl_notice("Serial port adopted\n");
            serial_wsi = wsi;   /* 保存串口 wsi 供其他回调使用 */
            break;

        case LWS_CALLBACK_RAW_RX_FILE:
            /* 串口有数据到达，广播给所有 WebSocket 客户端 */
            if (len > 0) {
                broadcast_to_websockets((unsigned char *)in, len);
            }
            break;

        case LWS_CALLBACK_RAW_CLOSE_FILE:
            lwsl_notice("Serial port closed\n");
            serial_wsi = NULL;
            break;

        default:
            break;
    }
    return 0;
}

/* -------------------- 协议列表 -------------------- */
static struct lws_protocols protocols[] = {
    {
        "ws",                  /* 协议名称，浏览器连接时需指定 */
        callback_ws,
        sizeof(void *),        /* 每个连接私有数据大小（这里用于存储用户指针） */
        1024,                  /* rx buffer size */
    },
    {
        "raw-serial",
        callback_raw_serial,
        0,
        0,
    },
    { NULL, NULL, 0, 0 }
};

/* -------------------- 主函数 -------------------- */
int main(int argc, char **argv) {
    struct lws_context_creation_info info;
    struct lws_context *context;
    struct lws_vhost *vhost;
    int serial_fd;
    const char *serial_device = "/dev/ttyUSB0";
    int baudrate = B115200;

    if (argc >= 2) serial_device = argv[1];
    if (argc >= 3) baudrate = atoi(argv[2]);

    /* 初始化 libwebsockets 日志 */
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, NULL);

    /* 创建上下文 */
    memset(&info, 0, sizeof(info));
    info.port = 8080;                     /* WebSocket 服务端口 */
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context = lws_create_context(&info);
    if (!context) {
        lwsl_err("lws_create_context failed\n");
        return 1;
    }

    vhost = lws_get_vhost(context, 0);

    /* 打开串口设备 */
    serial_fd = open_serial(serial_device, baudrate);
    if (serial_fd < 0) {
        lwsl_err("Cannot open serial port %s\n", serial_device);
        lws_context_destroy(context);
        return 1;
    }

    /* 将串口 fd 收养为 RAW 连接 */
    serial_wsi = lws_adopt_descriptor_vhost(vhost, "raw-serial",
                                            serial_fd,
                                            LWS_ADOPT_RAW_FILE_DESC,
                                            NULL);
    if (!serial_wsi) {
        lwsl_err("Failed to adopt serial fd\n");
        close(serial_fd);
        lws_context_destroy(context);
        return 1;
    }

    lwsl_notice("Server started on port 8080, serial device: %s\n", serial_device);

    /* 主事件循环 */
    while (1) {
        lws_service(context, 50);
    }

    lws_context_destroy(context);
    return 0;
}
