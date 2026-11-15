#ifndef SOS2_ARPA_INET_H
#define SOS2_ARPA_INET_H
#include <netinet/in.h>
#define INET_ADDRSTRLEN  16
#define INET6_ADDRSTRLEN 46
#ifdef __cplusplus
extern "C" {
#endif
int inet_pton(int, const char *, void *);
#ifdef __cplusplus
}
#endif
#endif
