/*
 * Telling apart the pieces a Media Source Extensions page appends (segments.c): an initialization
 * segment (MP4: ftyp and moov boxes; WebM: the EBML header and the Segment's head up to its first
 * Cluster) from media segments, which the engine needs to start a new demuxer at each
 * initialization segment and to put the last one back in front after an abort().
 */
#ifndef MEDIA_SEGMENTS_H
#define MEDIA_SEGMENTS_H

#include <stddef.h>
#include <stdint.h>

enum { SEG_UNKNOWN, SEG_MP4, SEG_WEBM };

/* The container a MIME type names ("video/mp4; codecs=..."). */
int seg_kind(const char *mime);
/* If data starts with an initialization segment: its length (the offset of the first media segment
 * in data, or len if there is none in it); 0 if it does not start with one. */
size_t seg_init_length(const uint8_t *data, size_t len, int kind);

#endif
