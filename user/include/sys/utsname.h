#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <flux/abi.h>

int uname(struct utsname *buf);

#ifdef __cplusplus
}
#endif

#endif
