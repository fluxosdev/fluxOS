/*
 * The sound devices: /dev/speaker (the PC speaker), /dev/dsp (digital audio
 * out, after OSS), /dev/mixer and /dev/opl (the FM synthesizer). The
 * requests and records are in <flux/abi.h>; this header is where OSS
 * programs look for them.
 */
#ifndef _SYS_SOUNDCARD_H
#define _SYS_SOUNDCARD_H

#include <flux/abi.h>
#include <sys/ioctl.h>

#endif
