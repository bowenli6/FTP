#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <setjmp.h>

#define BUSY            2
#define ERR             -1
#define OK              0
#define FILE            1
#define MAXLINE         128
#define MAXSLEEP        128
#define MAXARG          8
#define MAXFILESIZE     4096

char new_host[MAXLINE], new_port[MAXLINE];

static int open_clientfd(char *hostname, char *port);
static int connect_retry(int domain, int type, int protocol, const struct sockaddr *addr, socklen_t alen);
static void handle_termination(int signum);
static int parse(char *buf, char *argv[]);
static char *packmsg(int argc, char *argv[], int *zip);
static void waitmsg(int clientfd, int zip);
static int hello(int clientfd);

int main(int argc, char *argv[])
{       
        int i, clientfd;
        char *host, *port;
        char cmdline[MAXLINE];
        int argnum, zip;
        char *arglist[MAXARG];
        char *msg;

        if (argc != 3) {
                fprintf(stderr, "Invalid arguments!\n");
                return 1;
        }

        host = argv[1];
        port = argv[2];

        
        /* Register signal handler for termination signals. */
        signal(SIGINT, handle_termination);
        signal(SIGTERM, handle_termination);
        
        if ((clientfd = open_clientfd(host, port)) < 0) {
                return 2;
        }

        /* Print connection message */
        fprintf(stdout, "-----------------------------------------------------\n");
        fprintf(stdout, "Connected to the server (%s, %s) ; clientfd: %d\n", host, port, clientfd);

        if (!hello(clientfd)){
                printf("Connect to mirror (%s, %s)\n", new_host, new_port);
                if ((clientfd = open_clientfd(new_host, new_port)) < 0) {
                        return 2;
                }
        }
        
        /* Read command line arguments */
        while (1) {
                memset(cmdline, 0, MAXLINE);
                
                fprintf(stdout, "$ ");
                if (!fgets(cmdline, MAXLINE, stdin)) {
                        fprintf(stderr, "Read command line failed!\n");
                        return 3;
                }

                /* Parse command */
                if ((argnum = parse(cmdline, arglist)) < 0) {
                        fprintf(stderr, "parse from client %d: command not found.\n", clientfd);
                        continue;
                }

                if ((msg = packmsg(argnum, arglist, &zip))) {

                        /* send command to the server */
                        if (send(clientfd, msg, strlen(msg), 0) < 0) {
                                fprintf(stderr, "send failed!\n");
                                free(msg);
                                close(clientfd);
                                exit(1);
                        }

                        if (!strcmp(msg, "quit\n")) {
                                fprintf(stdout, "Client %d is quitting.\n", clientfd);
                                free(msg);
                                close(clientfd);
                                exit(0);        
                        }

                        free(msg);
                        
                        /* waiting for responses */
                        waitmsg(clientfd, zip);
                        
                } else {
                        fprintf(stderr, "packmsg: command not found.\n", clientfd);
                }
        }

        close(clientfd);
        fprintf(stdout, "Client has exited!\n");
        return 0;
}       


/**
 * @brief Create a new client socket and connect to the server
 * 
 * @param hostname : hostname for the server to connect
 * @param port : port number for the server to connect
 */
static int open_clientfd(char *hostname, char *port) {
        int clientfd, err;
        struct addrinfo *p, *listp;
        struct addrinfo hints;

        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_socktype = SOCK_STREAM;        /* TCP connection */
        hints.ai_flags = AI_NUMERICSERV;        /* Using numeric port number */
        hints.ai_flags |= AI_ADDRCONFIG;        /* Query for whichever address type is configured */

        if ((err = getaddrinfo(hostname, port, &hints, &listp)) != 0) {
                fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(err));
                exit(1);
        }

        /* Walk the list for one that we can successfully connect to */
        for (p = listp; p; p = p->ai_next) {
                if ((clientfd = connect_retry(p->ai_family, p->ai_socktype, p->ai_protocol, p->ai_addr, p->ai_addrlen)) < 0)
                        err = errno;    /* Failed! Try the next one */
                else 
                        break;          /* Success */
        }

        freeaddrinfo(listp);

        if (!p) {       /* All connects failed */
                fprintf(stderr, "can't connect to %s\n", hostname);
                return -1;
        }

        return clientfd;
}



/**
 * @brief Use the exponential backoff algorithm to keep trying
 * to connect to the server. If the call to connect fails, the
 * process goes to sleep for a short time and then tries again,
 * increasing the delay each time through the loop, up to a 
 * maximum delay of about 2 minutes.
 * 
 * @param domain: communication domain; this selects the protocol 
 * family which will be used for communication.
 * @param type : type of the socket (SOCK_STREAM for TCP protocol)
 * @param protocol : particular protocol if default protocol unused
 * @param addr : socket address of the server
 * @param alen : size of addr
 * @return int : If the connection or binding succeeds, zero is 
 * returned. On error, -1 is returned, and errno is set appropriately.
 * 
 */
static int connect_retry(int domain, int type, int protocol, const struct sockaddr *addr, socklen_t alen) 
{
        int sockfd, numsec;

        /* Try to connect with exponential backoff */
        for (numsec = 1; numsec <= MAXSLEEP; numsec <<= 1) {
                if ((sockfd = socket(domain, type, protocol)) < 0)
                        return -1;

                if (!connect(sockfd, addr, alen))
                        /* Connection accepted */
                        return sockfd;

                close(sockfd);

                /* Delay before trying again */
                if (numsec <= MAXSLEEP / 2)
                        sleep(numsec);
        }

        /* Failed! */
        return -1;
}


static void handle_termination(int signum) {
        fprintf(stdout, "Client terminated!\n");
        exit(0);
}



/**
 * @brief parse the command line
 *
 * @param buf : copy of the command line
 * @param argv : arguments list to be set
 * @return int : argc on success -1 otherwise
 */
static int parse(char *buf, char *argv[])
{
        char *delim;        /* points to the first space delimiter */
        int argc;           /* number of arguments */

        /* replace trailing \n with space */
        buf[strlen(buf) - 1] = ' ';

        /* skipping leading spaces */
        while (*buf && (*buf == ' ')) ++buf;

        /* build the argv list */
        argc = 0;
        while ((delim = strchr(buf, ' '))) {
                /* copy argument */
                argv[argc++] = buf;
                *delim = '\0';
                buf = delim + 1;

                /* skipping leading spaces */
                while (*buf && (*buf == ' ')) ++buf;
        }
        argv[argc] = NULL;

        /* blank line */
        if (!argc) return -1;
        
        return argc;
}


static char *packmsg(int argc, char *argv[], int *zip)
{
        int i;
        char *msg = malloc(argc * MAXLINE);

        /* default to zip the files */
        *zip = 1;

        /* check first argument */
        if (!strcmp(*argv, "findfile")) {
                /* findfile <filename> */
                if (argc != 2 || !argv[1]) 
                        goto error;

                sprintf(msg, "%s %s\n", argv[0], argv[1]);

        } else if (!strcmp(*argv, "sgetfiles") || !strcmp(*argv, "dgetfiles")) {
                /* s(d)getfiles <size1> <size2> <-u> */
                if (argc < 3 || argc > 4)
                        goto error;

                if (argc == 4) {
                        if (!strcmp(argv[3], "-u"))
                                *zip = 0;
                        else
                                goto error;
                }

                sprintf(msg, "%s %s %s\n", argv[0], argv[1], argv[2]);

        } else if (!strcmp(*argv, "getfiles") || !strcmp(*argv, "gettargz")) {
                if (argc < 2 || argc > 8)
                        goto error;
                strcpy(msg, argv[0]);
                
                if (argc == 8) {
                        if (!strcmp(argv[7], "-u")) 
                                *zip = 0;
                        else
                                goto error;
                }

                for (i = 1; i < argc; ++i) {
                        if (i == argc - 1 && !strcmp(argv[i], "-u")) {
                                *zip = 0;
                                break;
                        }
                        strcat(msg, " ");
                        strcat(msg, argv[i]);
                }
                strcat(msg, "\n");

        } else if (!strcmp(*argv, "quit")) {
                if (argc != 1)
                        goto error;
                
                sprintf(msg, "%s\n", argv[0]);

        } else {
                goto error;
        }

        return msg;

error:
        free(msg);
        return NULL;
}


static void waitmsg(int clientfd, int zip) 
{
        int status = ERR;
        int fd, nrecv;
        char buf[MAXFILESIZE];
        char *fp = buf;
        char err_str[MAXLINE];

        fd = open("temp.tar.gz", O_RDWR | O_CREAT | O_TRUNC, 0644);
        int first = 1;
        int fsize = 0;

        while ((nrecv = recv(clientfd, fp, sizeof(buf), 0)) > 0) {                
                if (first) {
                        if (nrecv > 3 && !strncmp(buf, "OK:", 3)) {
                                status = OK;
                                break;
                        } else if (nrecv > 4 && !strncmp(buf, "ERR:", 4)) {
                                status = ERR;
                                break;
                        } else if (nrecv > 5) {
                                char *p;
                                p = strchr(buf, '\n');
                                if (p && !strncmp(buf, "SIZE:", 5)) {
                                        *p = '\0';
                                        status = FILE;
                                        fsize = atoi(buf + 5);
                                        fp = p + 1;
                                        nrecv -= strlen(buf) + 1;
                                        first = 0;
                                }
                        } else {
                                break;
                        }
                }

                if (status == FILE) {
                        write(fd, fp, nrecv);
                        fsize -= nrecv;
                }

                if (status == FILE && !fsize)
                        break;
        }

        if (nrecv < 0) {
                fprintf(stderr, "recv from server error\n");
                return;
        }


        switch (status) {
                case ERR:
                        /* print the error message to the screen */
                        fprintf(stderr, "%s\n", buf + 4);
                        unlink("temp.tar.gz");
                        break;
                case OK:
                        /* print the result to the screen */
                        fprintf(stdout, "%s", buf + 3);
                        unlink("temp.tar.gz");
                        break;
                case FILE:
                        if (!zip) {
                                system("tar -xzf temp.tar.gz -C .");
                                unlink("temp.tar.gz");
                        }
                        break;
                
        }

        close(fd);
}

static int hello(int clientfd)
{
        int nrecv;
        char msg[MAXLINE];
        char buf[MAXLINE];
        strcpy(msg, "HELLO\n");

        printf("Hello server!\n");
        if (send(clientfd, msg, strlen(msg), 0) < 0) {
                fprintf(stderr, "send failed!\n");
                close(clientfd);
                exit(1);
        }

        if ((nrecv = recv(clientfd, buf, sizeof(buf), 0)) > 0) {  

                printf("From server: %s\n", buf);
                if (nrecv == 3 && !strcmp(buf, "OK")) {
                        return 1;
                } else {
                        /* buf: BUSY:host_name port\0*/
                        char *p = buf + 5;
                        char *delm = strchr(p, ' ');

                        *delm = '\0';
                        strcpy(new_host, p);
                        strcpy(new_port, delm + 1);

                        strcpy(msg, "quit\n");

                        if (send(clientfd, msg, strlen(msg), 0) < 0) {
                                fprintf(stderr, "send failed!\n");
                                close(clientfd);
                                exit(1);
                        }

                        close(clientfd);
                        return 0;
                }   
        } else {
                fprintf(stderr, "recv from server error\n");
                close(clientfd);
                exit(1);
        }          
}
