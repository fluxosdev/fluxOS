/*
 * Sound files Wave can play (codecs.c): PCM WAV, MP3 (minimp3) and Ogg Vorbis (stb_vorbis), decoded
 * from a file held in memory to interleaved signed 16-bit frames.
 */
#ifndef CODECS_H
#define CODECS_H

#include <stddef.h>
#include <stdint.h>

struct codec;

/* What kind of sound file data looks like: "audio/wav", "audio/mpeg", "audio/ogg", or NULL. */
const char *codec_sniff(const unsigned char *data, size_t len);
/* Is a MIME type (with its codecs= parameter) one of those? 2 surely, 1 perhaps, 0 not. */
int codec_type_playable(const char *type);
/* A decoder for data, which must outlive it; NULL if it is none of the kinds above. */
struct codec *codec_open(const unsigned char *data, size_t len);
int codec_rate(const struct codec *c);
int codec_channels(const struct codec *c);         /* 1 or 2 */
uint32_t codec_frames(const struct codec *c);      /* how long it is (0 if that is not known) */
/* The next frames (up to max, codec_channels samples each); 0 at the end. */
int codec_read(struct codec *c, int16_t *out, int max);
int codec_seek(struct codec *c, uint32_t frame);
void codec_close(struct codec *c);

#endif
