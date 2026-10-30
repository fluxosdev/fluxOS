/*
 * mixer - show or set the Sound Blaster 16's volumes
 *
 *   mixer                      show the card and every volume
 *   mixer CHANNEL LEVEL [RIGHT]
 *
 * CHANNEL is master, pcm (digital audio), synth (FM), cd, line, mic or
 * speaker (the PC speaker's input); levels go from 0 to 100, and a
 * stereo channel takes a second level for the right side.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <unistd.h>

static const char *const names[MIXER_CHANNELS] = {
    [MIXER_MASTER] = "master", [MIXER_PCM] = "pcm", [MIXER_SYNTH] = "synth", [MIXER_CD] = "cd",
    [MIXER_LINE] = "line", [MIXER_MIC] = "mic", [MIXER_SPEAKER] = "speaker",
};

static void show(int fd)
{
    struct sound_info si;
    if (ioctl(fd, SOUND_INFO, &si) == 0 && (si.flags & SOUNDF_HDA)) {
        printf("%s (HD Audio), IRQ %d", si.card, si.irq);
        if (si.flags & SOUNDF_OPL2)
            printf("; %s at %Xh", (si.flags & SOUNDF_OPL3) ? "OPL3" : "OPL2", si.opl_base);
        printf("\n");
    } else if (ioctl(fd, SOUND_INFO, &si) == 0) {
        printf("%s, DSP %d.%02d at %Xh, IRQ %d, DMA %d", si.card, si.dsp_version >> 8,
               si.dsp_version & 0xFF, si.base, si.irq, si.dma8);
        if (si.dma16 != 0xFF)
            printf(" and %d", si.dma16);
        if (si.flags & SOUNDF_OPL2)
            printf("; %s at %Xh", (si.flags & SOUNDF_OPL3) ? "OPL3" : "OPL2", si.opl_base);
        printf("\n");
    }
    for (int c = 0; c < MIXER_CHANNELS; c++) {
        struct mixer_level ml = { (uint32_t)c, 0, 0 };
        if (ioctl(fd, MIXER_READ, &ml) < 0)
            continue;
        printf("%-8s %3u", names[c], ml.left);
        if (c <= MIXER_LINE)
            printf(" %3u", ml.right);
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    int fd = open("/dev/mixer", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "mixer: /dev/mixer: %s\n",
                errno == ENOENT ? "no Sound Blaster 16" : strerror(errno));
        return 1;
    }
    if (argc == 1) {
        show(fd);
        return 0;
    }
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: mixer [CHANNEL LEVEL [RIGHT]]\n");
        return 2;
    }
    int c = 0;
    while (c < MIXER_CHANNELS && strcmp(names[c], argv[1]))
        c++;
    if (c == MIXER_CHANNELS) {
        fprintf(stderr, "mixer: no channel called %s\n", argv[1]);
        return 2;
    }
    struct mixer_level ml = { (uint32_t)c, (uint32_t)atoi(argv[2]), 0 };
    ml.right = argc == 4 ? (uint32_t)atoi(argv[3]) : ml.left;
    if (ioctl(fd, MIXER_WRITE, &ml) < 0) {
        perror("mixer");
        return 1;
    }
    close(fd);
    return 0;
}
