#ifndef SOS2_SERVICES_H
#define SOS2_SERVICES_H
#include <user_abi.h>
#include <user_input.h>
#include <user_browser.h>
#ifdef __cplusplus
extern "C" {
#endif
int sos2_memory_info(struct user_memory_info *);
/* Resolve relative components without requiring the final path to exist. */
int sos2_absolute_path(const char *, char out[128]);
int sos2_present(const uint32_t *, uint32_t bytes);
int sos2_present_damage(const uint32_t *, unsigned x, unsigned y, unsigned width, unsigned height);
int sos2_present_frame(const uint32_t *, const struct user_pixel_scroll *,
                       const struct user_pixel_rect *, unsigned count);
int sos2_surface_state(void);
/* SYS_GPU (include/user_abi.h): argument is a struct address, or a handle for
 * USER_GPU_DESTROY. Returns a non-negative result or a negative errno. */
int sos2_gpu(unsigned op, uint32_t argument, uint32_t size);
/* The errno value for a negative SOS2 result, such as sos2_gpu's. */
int sos2_errno(int result);
/* Futex-style wait: sleep while *word equals expected, for at most us
 * microseconds (USER_WAIT_FOREVER: no limit). Returns 0 once woken, or
 * USER_EAGAIN if *word differed, or USER_ETIMEDOUT. */
int sos2_wait(uint32_t *word, uint32_t expected, uint32_t us);
/* Wake at most count threads waiting on word. */
void sos2_wake(uint32_t *word, unsigned count);
/* 1 = event, 0 = empty; -1 with errno for a bad destination. */
int sos2_next_event(struct user_input_event *);
/* Request the trusted touch keyboard for this page's focused editable field. */
int sos2_input_method(int enabled);
/* Take the Back key (the phone's key below the display; Alt+Left in QEMU):
 * 1 delivers it as an INPUT_KEY press and release of code 158 (KEY_BACK).
 * 0, the default for every new process, leaves it to the desktop: a page
 * goes back in its history and an app returns to the desktop. */
int sos2_back_key(int enabled);
/* A short buzz of the phone's vibrator (QEMU has none), for a finger landing
 * on one of the app's own buttons: once for each press the app was sent (four
 * at most wait), until 250 ms after the latest. Returns 0, or -1 with errno
 * EAGAIN when no press waits for it. */
int sos2_haptic_tap(void);
/* Lock a /data or /tmp path against other processes; 0 releases it. At most
 * one process holds a path, a process at most four, and a lock ends with its
 * process. Taking a lock reopens this process's open file if another process
 * wrote it since; releasing one commits this process's writes. Returns -1
 * with errno EBUSY while another process holds it, or EAGAIN when no lock is
 * left. SQLite uses this for databases that the pages sharing the browser
 * profile open together. */
int sos2_file_lock(const char *path, int locked);
/* SYS_UPDATE (include/user_abi.h), for the system's updater alone; INFO fills data.
 * Returns 0, or a negative USER_E* value: USER_EAGAIN while FINISH has more to do. */
int sos2_update(unsigned op, void *data, uint32_t size);
int sos2_browser_command(struct user_browser_command *);
int sos2_browser_state(const struct user_browser_state *);
#ifdef __cplusplus
}
#endif
#endif
