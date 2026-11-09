#ifndef WIN_AUDIO_H
#define WIN_AUDIO_H
#include "win32.h"
#define AUDIO_RATE 44100
struct audio_format {uint16_t tag,channels;uint32_t rate,average;uint16_t block,bits,extra;} __attribute__((packed));
struct audio_source {
    struct audio_source *next;
    uint32_t id;
    int refs,attached;
    int (*sample)(struct audio_source *,int *,int *);
    void (*poll)(struct audio_source *,uint64_t played);
    void (*destroy)(struct audio_source *);
};
int audio_available(void);
int audio_attach(struct audio_source *s);
void audio_detach(struct audio_source *s);
void audio_ref(struct audio_source *s);
void audio_unref(struct audio_source *s);
uint64_t audio_frame(void);
int audio_format_read(gp_t ptr,struct audio_format *out);
void audio_pcm_sample(const struct audio_format *f,gp_t data,uint32_t frame,int *l,int *r);
extern const struct api_entry waveout_entries[];
extern const int waveout_count;
#endif
