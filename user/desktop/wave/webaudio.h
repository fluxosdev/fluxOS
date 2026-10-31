/*
 * Sound for Wave's <audio> (webaudio.c). /dev/dsp takes one program at a time, so one process,
 * "wave --audio", plays everything the browser's pages play: it decodes each stream (codecs.c),
 * mixes them, and keeps the device only while something sounds. The browser talks to it through
 * the functions below; what it says back comes to the handler.
 */
#ifndef WEBAUDIO_H
#define WEBAUDIO_H

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
/* The player process itself: "wave --audio". */
int webaudio_main(void);

#endif
