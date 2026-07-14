#pragma once

#define NO_SYS                      1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_DHCP                   1
#define LWIP_DNS                    1
#define LWIP_RAW                    1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_STATS                  0
#define LWIP_PROVIDE_ERRNO          1

#define MEM_SIZE                    (24 * 1024)
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            32
#define PBUF_POOL_SIZE              16
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            32
