// SPDX-License-Identifier: Apache-2.0
// Emulate Linux 4.9 accepting UDP_SEGMENT while ignoring its control message.
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifndef SOL_UDP
#define SOL_UDP 17
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

typedef ssize_t (*SendMessage)(int, const struct msghdr *, int);

ssize_t sendmsg(int socket, const struct msghdr *message, int flags) {
    SendMessage sendMessage = (SendMessage)dlsym(RTLD_NEXT, "sendmsg");
    if (!sendMessage) {
        errno = ENOSYS;
        return -1;
    }

    // The FFL GSO path supplies exactly one control message. Leave all other
    // socket calls untouched and let the real kernel transmit the datagram.
    struct msghdr outgoing = *message;
    struct cmsghdr *control = CMSG_FIRSTHDR(&outgoing);
    if (control && control->cmsg_level == SOL_UDP && control->cmsg_type == UDP_SEGMENT &&
        CMSG_NXTHDR(&outgoing, control) == NULL) {
        static atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!atomic_flag_test_and_set(&reported)) {
            fprintf(stderr, "UDP_SEGMENT ignored by test kernel\n");
        }

        outgoing.msg_control = NULL;
        outgoing.msg_controllen = 0;
    }

    return sendMessage(socket, &outgoing, flags);
}
