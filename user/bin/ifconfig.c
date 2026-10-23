/*
 * ifconfig - show or set the network interface's address.
 *
 *   ifconfig                                   show the interface
 *   ifconfig ne0 ADDR [netmask M] [gw G] [dns D]   set a static address
 *   ifconfig ne0 dhcp                          ask DHCP for an address
 */
#include <arpa/inet.h>
#include <flux.h>
#include <stdio.h>
#include <string.h>

static const char *ip(uint32_t addr)
{
    struct in_addr a = { addr };
    return inet_ntoa(a);
}

static void human(uint32_t bytes, char *out, size_t size)
{
    if (bytes >= 10u << 20)
        snprintf(out, size, "%u.%u MiB", bytes >> 20, ((bytes >> 10) & 1023) * 10 / 1024);
    else if (bytes >= 10u << 10)
        snprintf(out, size, "%u.%u KiB", bytes >> 10, (bytes & 1023) * 10 / 1024);
    else
        snprintf(out, size, "%u B", bytes);
}

static int show(void)
{
    struct netinfo ni;
    if (netinfo(&ni) < 0) {
        perror("ifconfig");
        return 1;
    }
    printf("%s: flags=<%s%s%s>  driver %s\n", ni.name, ni.flags & NETF_UP ? "UP" : "DOWN",
           ni.flags & NETF_DHCP ? ",DHCP" : "", ni.flags & NETF_BOUND ? ",RUNNING" : "", ni.driver);
    if (!(ni.flags & NETF_UP)) {
        printf("        no network card; only 127.0.0.1 (loopback) works\n");
        return 0;
    }
    if (ni.flags & NETF_BOUND) {
        printf("        inet %s", ip(ni.addr));
        printf("  netmask %s", ip(ni.netmask));
        printf("  gateway %s\n", ip(ni.gateway));
        printf("        dns %s", ni.dns ? ip(ni.dns) : "none");
        if (ni.lease_secs)
            printf("  lease %uh %02um left", ni.lease_secs / 3600, ni.lease_secs / 60 % 60);
        printf("\n");
    } else {
        printf("        no address%s\n", ni.flags & NETF_DHCP ? " yet (waiting for DHCP)" : "");
    }
    printf("        ether %02x:%02x:%02x:%02x:%02x:%02x  io 0x%x  irq %u\n", ni.mac[0], ni.mac[1],
           ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5], ni.io_base, ni.irq);
    char rx[24], tx[24];
    human(ni.rx_bytes, rx, sizeof(rx));
    human(ni.tx_bytes, tx, sizeof(tx));
    printf("        RX packets %u  bytes %u (%s)  errors %u  dropped %u\n", ni.rx_packets,
           ni.rx_bytes, rx, ni.rx_errors, ni.rx_dropped);
    printf("        TX packets %u  bytes %u (%s)  errors %u\n", ni.tx_packets, ni.tx_bytes, tx,
           ni.tx_errors);
    return 0;
}

static int parse(const char *s, uint32_t *out)
{
    struct in_addr a;
    if (!inet_aton(s, &a)) {
        fprintf(stderr, "ifconfig: %s: not an IPv4 address\n", s);
        return -1;
    }
    *out = a.s_addr;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && (!strcmp(argv[1], "ne0") || !strcmp(argv[1], "-a"))))
        return show();
    if (strcmp(argv[1], "ne0") || argc < 3) {
        fprintf(stderr, "usage: ifconfig [ne0 ADDR [netmask M] [gw G] [dns D] | ne0 dhcp]\n");
        return 2;
    }
    struct netinfo ni;
    if (netinfo(&ni) < 0) {
        perror("ifconfig");
        return 1;
    }
    if (!strcmp(argv[2], "dhcp")) {
        ni.flags = NETF_DHCP;
    } else {
        ni.flags = 0;
        if (parse(argv[2], &ni.addr) < 0)
            return 1;
        ni.netmask = htonl(0xFFFFFF00u);
        for (int i = 3; i + 1 < argc; i += 2) {
            uint32_t *field = !strcmp(argv[i], "netmask") ? &ni.netmask
                            : !strcmp(argv[i], "gw") ? &ni.gateway
                            : !strcmp(argv[i], "dns") ? &ni.dns : NULL;
            if (!field) {
                fprintf(stderr, "ifconfig: unknown option '%s'\n", argv[i]);
                return 2;
            }
            if (parse(argv[i + 1], field) < 0)
                return 1;
        }
    }
    if (netconf(&ni) < 0) {
        perror("ifconfig");
        return 1;
    }
    return 0;
}
