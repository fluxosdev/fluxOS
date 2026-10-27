/* Image decoding outside the desktop process. Completions run on the UI loop.
 * owner is a cancellation token: cancel it before destroying its window/page.
 * Accepted jobs return 0; their completion is always deferred and owns image.
 * cleanup(context) runs after completion or cancellation, exactly once.
 * A rejected job returns -1 and leaves context with its caller. */
#ifndef IMAGEJOBS_H
#define IMAGEJOBS_H
#include "gfx.h"
typedef void (*image_job_done)(void *context, struct image *image, int full_w, int full_h);
void image_workers_init(void);
void image_workers_shutdown(void);
void image_jobs_poll(void);
int image_jobs_timeout(void);
int image_worker_child_exited(int pid);
int image_job_path(void *owner, const char *path, int thumbnail, image_job_done done,
                   void *context, void (*cleanup)(void *));
int image_job_data(void *owner, const void *data, unsigned length, image_job_done done,
                   void *context, void (*cleanup)(void *));
int image_job_icon(void *owner, const void *data, unsigned length, image_job_done done,
                   void *context, void (*cleanup)(void *));
int image_job_url(void *owner, const char *url, image_job_done done,
                  void *context, void (*cleanup)(void *));
int image_job_wallpaper(void *owner, const char *path, int mode, uint32_t color, int w, int h,
                        image_job_done done, void *context, void (*cleanup)(void *));
void image_jobs_cancel(void *owner);
#endif
