#ifndef SOS2_SYSLOG_H
#define SOS2_SYSLOG_H
#include <stdarg.h>
/* There is no system log: messages are discarded. */
#define LOG_EMERG                       0
#define LOG_ALERT                       1
#define LOG_CRIT                        2
#define LOG_ERR                         3
#define LOG_WARNING                     4
#define LOG_NOTICE                      5
#define LOG_INFO                        6
#define LOG_DEBUG                       7
#define LOG_PRIMASK                     0x07
#define LOG_PRI(p)                      ((p) & LOG_PRIMASK)
#define LOG_MAKEPRI(facility, priority) ((facility) | (priority))
#define LOG_KERN                        (0 << 3)
#define LOG_USER                        (1 << 3)
#define LOG_DAEMON                      (3 << 3)
#define LOG_LOCAL0                      (16 << 3)
#define LOG_LOCAL1                      (17 << 3)
#define LOG_LOCAL2                      (18 << 3)
#define LOG_LOCAL3                      (19 << 3)
#define LOG_LOCAL4                      (20 << 3)
#define LOG_LOCAL5                      (21 << 3)
#define LOG_LOCAL6                      (22 << 3)
#define LOG_LOCAL7                      (23 << 3)
#define LOG_MASK(priority)              (1 << (priority))
#define LOG_UPTO(priority)              ((1 << ((priority) + 1)) - 1)
#define LOG_PID                         0x01
#define LOG_CONS                        0x02
#define LOG_ODELAY                      0x04
#define LOG_NDELAY                      0x08
#define LOG_NOWAIT                      0x10
#define LOG_PERROR                      0x20
#ifdef __cplusplus
extern "C" {
#endif
void openlog(const char *ident, int option, int facility);
void closelog(void);
int setlogmask(int mask);
void syslog(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
void vsyslog(int priority, const char *format, va_list arguments);
#ifdef __cplusplus
}
#endif
#endif
