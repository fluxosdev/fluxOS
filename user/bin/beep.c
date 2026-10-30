/*
 * beep - make sounds on the PC speaker
 *
 *   beep [-f HZ] [-l MS] [-r REPEATS] [-d MS] [-n ...]
 *   beep -m "MUSIC"
 *
 * -f sets the frequency (440 Hz), -l the length (200 ms), -r how many times
 * (1) and -d the pause between repeats (100 ms); -n starts another beep
 * with its own options. -m plays a tune in the music language of BASIC's
 * PLAY statement: notes A-G (# or + sharp, - flat, then an optional length
 * and dots), N0-84 by number, P or R for a rest, O0-6 and < > for the
 * octave, L1-64 for the length of notes, T32-255 for the tempo in quarter
 * notes a minute, and MS, MN, ML for staccato, normal and legato. Octave 3
 * starts at middle C.
 * beep returns when the sound has finished.
 */
#include <ctype.h>
#include <fcntl.h>
#include <flux.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <unistd.h>

#define MAX_TONES 1024

static struct spkr_tone tones[MAX_TONES];
static int ntones;

static void add(uint32_t freq, uint32_t ms)
{
    if (ntones < MAX_TONES && ms) {
        tones[ntones].freq = freq;
        tones[ntones].ms = ms;
        ntones++;
    }
}

/* Equal temperament. Note n is semitone n % 12 of octave n / 12, and octave 3
 * starts at middle C, as in BASIC; the frequencies of the highest octave
 * of a piano (C8-B8, in hundredths of a hertz) halve down to it. */
static uint32_t note_freq(int n)
{
    static const uint32_t top[12] = {
        418601, 443492, 469864, 497803, 527404, 558765, 591991, 627193, 664488, 704000, 745862, 790213,
    };
    if (n < 0 || n > 83)
        return 0;
    return ((top[n % 12] >> (7 - n / 12)) + 50) / 100;
}

static int number(const char **p, int def)
{
    if (!isdigit((unsigned char)**p))
        return def;
    int v = 0;
    while (isdigit((unsigned char)**p))
        v = v * 10 + (*(*p)++ - '0');
    return v;
}

static int play_music(const char *s)
{
    int octave = 4, length = 4, tempo = 120, style = 7;     /* style: eighths sounded */
    static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };    /* A B C D E F G */
    while (*s) {
        int c = toupper((unsigned char)*s++);
        int note = -1, len = length;
        if (c >= 'A' && c <= 'G') {
            note = octave * 12 + semis[c - 'A'];
            if (*s == '#' || *s == '+') {
                note++;
                s++;
            } else if (*s == '-') {
                note--;
                s++;
            }
            len = number(&s, length);
        } else if (c == 'N') {
            int n = number(&s, 0);
            note = n ? n - 1 : -2;      /* N0 is a rest */
        } else if (c == 'P' || c == 'R') {
            note = -2;
            len = number(&s, length);
        } else if (c == 'O') {
            octave = number(&s, octave);
            if (octave > 6)
                octave = 6;
            continue;
        } else if (c == '<') {
            if (octave > 0)
                octave--;
            continue;
        } else if (c == '>') {
            if (octave < 6)
                octave++;
            continue;
        } else if (c == 'L') {
            length = number(&s, length);
            if (length < 1 || length > 64)
                length = 4;
            continue;
        } else if (c == 'T') {
            tempo = number(&s, tempo);
            if (tempo < 32 || tempo > 255)
                tempo = 120;
            continue;
        } else if (c == 'M') {
            int m = toupper((unsigned char)*s);
            if (m == 'S' || m == 'N' || m == 'L')
                style = m == 'S' ? 6 : m == 'N' ? 7 : 8;
            if (*s)
                s++;
            continue;
        } else {
            continue;                   /* spaces, commas, anything unknown */
        }
        if (len < 1 || len > 64)
            len = length;
        /* A whole note is four beats; each dot adds half of what is left. */
        uint32_t ms = 240000u / (uint32_t)tempo / (uint32_t)len, extra = ms / 2;
        while (*s == '.') {
            ms += extra;
            extra /= 2;
            s++;
        }
        if (note < 0) {
            add(0, ms);
        } else {
            uint32_t on = ms * (uint32_t)style / 8;
            add(note_freq(note), on);
            add(0, ms - on);
        }
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: beep [-f HZ] [-l MS] [-r REPEATS] [-d MS] [-n ...]\n"
                    "       beep -m MUSIC\n");
    exit(2);
}

int main(int argc, char **argv)
{
    uint32_t freq = 440, len = 200, reps = 1, delay = 100;
    int pending = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-n")) {
            for (uint32_t r = 0; r < reps; r++) {
                add(freq, len);
                if (r + 1 < reps)
                    add(0, delay);
            }
            add(0, delay);
            freq = 440, len = 200, reps = 1, delay = 100;
            continue;
        }
        if (a[0] != '-' || !a[1] || a[2] || i + 1 >= argc)
            usage();
        const char *v = argv[++i];
        switch (a[1]) {
        case 'f': freq = (uint32_t)atoi(v); break;
        case 'l': len = (uint32_t)atoi(v); break;
        case 'r': reps = (uint32_t)atoi(v); break;
        case 'd': delay = (uint32_t)atoi(v); break;
        case 'm': play_music(v); pending = 0; break;
        default: usage();
        }
    }
    if (pending) {
        for (uint32_t r = 0; r < reps; r++) {
            add(freq, len);
            if (r + 1 < reps)
                add(0, delay);
        }
    }

    int fd = open("/dev/speaker", O_WRONLY);
    if (fd < 0) {
        perror("beep: /dev/speaker");
        return 1;
    }
    uint32_t total = 0;
    for (int i = 0; i < ntones; i++)
        total += tones[i].ms;
    if (write(fd, tones, (unsigned int)ntones * sizeof(tones[0])) < 0) {
        perror("beep");
        return 1;
    }
    /* The queue plays in the background; stay until it has. ^C stops it. */
    signal(SIGINT, SIG_DFL);
    if (msleep(total) < 0)
        ioctl(fd, SPKR_STOP);
    close(fd);
    return 0;
}
