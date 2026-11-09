/*
 * Sound for Wave's <audio> (webaudio.c). /dev/dsp takes one program at a time, so one process,
 * "wave --audio", plays everything the browser's pages play: it decodes each stream (codecs.c),
 * mixes them, and keeps the device only while something sounds. The browser talks to it through
 * the functions below; what it says back comes to the handler.
 */
#ifndef WEBAUDIO_H
#define WEBAUDIO_H

#include <stdint.h>

/* What the player says about stream id: "meta" (a: its length in ms, -1 if not known), "time"
 * (a: where it is, ms), "ended", "error". */
typedef void (*webaudio_handler)(int id, const char *what, int a);
void webaudio_set_handler(webaudio_handler fn);
/* Give the player a sound file for stream id (it reads, then deletes it); 0 if it was sent. */
int webaudio_open(int id, const char *path);
void webaudio_play(int id, int from_ms);       /* from_ms < 0: from where it is */
void webaudio_pause(int id);
void webaudio_seek(int id, int ms);
void webaudio_volume(int id, int per_mille);
void webaudio_loop(int id, int on);
void webaudio_close(int id);
/* The player process itself: "wave --audio", or the media engine (/bin/wavemedia, which plays
 * <video> and Media Source Extensions streams too and is started in its place when it is there). */
int webaudio_main(void);

/* Lines for the media engine ("vplay 3 0\n"...), and what it says that is not about <audio> streams
 * ("vframe ...", "sbdone ..."), which goes to the line handler. 0 if it was sent. */
int webaudio_send(const char *line);
typedef void (*webaudio_line_handler)(const char *line);
void webaudio_set_line_handler(webaudio_line_handler fn);
/* Whether the player running is the media engine (known once it has started). */
int webaudio_engine(void);

/* The engine's hooks into the player's loop, all called on the loop's thread. */
struct webaudio_ext {
    int (*command)(char *line);         /* 1 if it took the line */
    int (*want_sound)(void);            /* something of its plays sound now: mix() must be called */
    int (*busy)(void);                  /* something of its runs: tick() often */
    /* Add frames of its sound into acc (stereo, at rate); delay: frames the card has yet to play. */
    void (*mix)(int32_t *acc, int frames, int rate, int delay);
    void (*tick)(void);
};
void webaudio_set_ext(const struct webaudio_ext *ext);
/* Say a line to the browser (from any of the player's threads). */
void webaudio_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
