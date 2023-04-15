#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#define MAXLINE 128

int main(int argc, char *argv[])
{
        struct addrinfo *p, *listp;
        struct addrinfo hints;
        char host[MAXLINE];
        char service[MAXLINE];
        int rc, flags;

        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_family = AF_INET;
        hints.ai_flags = AI_CANONNAME;
        hints.ai_socktype = SOCK_STREAM;
        if ((rc = getaddrinfo(argv[1], "http", &hints, &listp)) != 0) {
                fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(rc));
                exit(1);
        }

        flags = NI_NUMERICHOST | NI_NUMERICSERV;
        for (p = listp; p; p = p->ai_next) {
                getnameinfo(p->ai_addr, p->ai_addrlen, host, MAXLINE, service, MAXLINE, flags);
                printf("%s\n", p->ai_canonname);
                printf("%s:%s\n", host, service);
        }

        freeaddrinfo(listp);
        return 0;
}
