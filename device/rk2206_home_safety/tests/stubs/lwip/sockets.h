#ifndef TEST_STUB_SOCKETS_H
#define TEST_STUB_SOCKETS_H
#include <sys/socket.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define lwip_ioctl ioctl
#define lwip_close close

#endif
