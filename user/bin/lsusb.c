/* lsusb - list USB devices */
#include <flux.h>
#include <stdio.h>
#include <string.h>

static const char *class_name(int cls)
{
    switch (cls) {
    case 0x01: return "audio";
    case 0x02: return "comms";
    case 0x03: return "HID";
    case 0x06: return "imaging";
    case 0x07: return "printer";
    case 0x08: return "storage";
    case 0x09: return "hub";
    case 0x0A: return "CDC data";
    case 0x0B: return "smart card";
    case 0x0E: return "video";
    case 0xE0: return "wireless";
    case 0xFF: return "vendor";
    }
    return "-";
}

int main(int argc, char **argv)
{
    static const char *speeds[] = { "1.5M", "12M", "480M", "5G" };
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    struct usbinfo ui;
    int n = 0;
    for (int i = 0; usbinfo(i, &ui) == 0; i++, n++) {
        char name[96];
        if (ui.name[0])
            snprintf(name, sizeof(name), "%s%s%s", ui.manufacturer, ui.manufacturer[0] ? " " : "",
                     ui.name);
        else
            snprintf(name, sizeof(name), "(no name)");
        printf("Bus %03u Device %03u: ID %04x:%04x %s\n", ui.bus, ui.addr, ui.vendor, ui.product,
               name);
        if (verbose)
            printf("    %s port %u%s, %s, class %02x (%s), driver %s\n", ui.hc, ui.port,
                   ui.depth ? " (behind a hub)" : "", ui.speed < 4 ? speeds[ui.speed] : "?",
                   ui.cls, class_name(ui.cls), ui.driver[0] ? ui.driver : "none");
    }
    if (!n)
        printf("No USB devices.\n");
    return 0;
}
