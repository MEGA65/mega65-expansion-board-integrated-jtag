#ifndef M65J_LWIPOPTS_H
#define M65J_LWIPOPTS_H

#define NO_SYS                          1
#define MEM_ALIGNMENT                   4

#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_DHCP                       1
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define DHCP_DOES_ARP_CHECK             0
#define LWIP_DNS                        1
#define LWIP_RAW                        1
#define LWIP_TCP                        1
#define LWIP_UDP                        1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_TX_SINGLE_PBUF       1
#define LWIP_STATS                      0
#define LWIP_PROVIDE_ERRNO              1

#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_SIZE                        (24 * 1024)

#define MEMP_NUM_RAW_PCB                4
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_ARP_QUEUE              10
#define PBUF_POOL_SIZE                  24

#define TCP_MSS                         1460
#define TCP_WND                         (8 * TCP_MSS)
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

// Default lwIP initial RTO is 3000ms, which makes brief WiFi loss look like a
// hang during core downloads. Keep the normal backoff behaviour, but start
// retransmission sooner on our local-control LAN use case.
#define LWIP_TCP_RTO_TIME               1000

#endif
