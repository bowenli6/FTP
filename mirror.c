#include <errno.h>
#include <stdio.h>
#include <time.h>

#include <ftw.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <libgen.h>

#define MAXSLEEP        128
#define ERR             -1
#define OK              10
#define FILE            11
#define QUIT            12
#define MIRROR          13
#define BUSY            14
#define QLEN            5
#define MAXARG          8
#define REQCNT          4
#define MAXLINE         128
#define MAXFILESIZE     4096

#define PATH            "data"


typedef struct {
        int nclient;
        int listenfd;
} socketfd_t;

socketfd_t socketfd;

char **extr_arg;
char message[MAXLINE];

int status;

static void recv_files(int clientfd, char *port);
static int open_clientfd(char *hostname, char *port);
static int open_listenfd(char *port);
static int connect_retry(int domain, int type, int protocol, const struct sockaddr *addr, socklen_t alen);
static int server_init(int type, const struct sockaddr *addr, socklen_t alen, int backlog);
static void send_file(int fd, int connfd);
static void send_text(char *msg, int connfd);
static void processclient(int listenfd);
static int set_cloexec(int fd);
static void sigchld_handler(int signum);
static void process(int connfd);
static int parse(char *buf, char *argv[MAXARG]);
static int eval(char *msg, int size);
static int compare(const struct stat *st, void *c1, void *c2, char *type);
static int contains(char *args[], char *fname);
static int get_file_ext(const char *fname, char *ext);
static int match(char *args[], const char *fname);

int findfile(const char *fpath, const struct stat *st, int type);
int sdgetfiles(const char *fpath, const struct stat *st, int type);

int main(int argc, char *argv[])
{
        char *port;
        char *server_hostname;
        char *server_port;
        int clientfd;
        
        if (argc != 4) {
                fprintf(stderr, "Invalid arguments!\n");
                return 1;
        }

        port = argv[1];
        server_hostname = argv[2];
        server_port = argv[3];

        if ((clientfd = open_clientfd(server_hostname, server_port)) < 0) {
                return 2;
        }

        printf("Ready to ask server for files...\n");
        recv_files(clientfd, port);

        if ((socketfd.listenfd = open_listenfd(port)) < 0) {
                return 3;
        }

        fprintf(stdout, "Ready to listen for connections...\n");

        /* set up signal handler */
        signal(SIGCHLD, sigchld_handler);

        /* start listening for events */
        processclient(socketfd.listenfd);
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


static void recv_files(int clientfd, char *port) 
{
        char msg[MAXLINE];
        sprintf(msg, "MIRROR %s\n", port);

        /* send mirror request to the server */
        if (send(clientfd, msg, strlen(msg), 0) < 0) {
                fprintf(stderr, "send failed!\n");
                close(clientfd);
                exit(1);
        }

        int fd, nrecv;
        char buf[MAXFILESIZE];

        fd = open("files.tar.gz", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        
        int first = 1;

        char *fp = buf;
        int fsize = 0;

        while ((nrecv = recv(clientfd, fp, sizeof(buf), 0)) > 0) {
                if (first && nrecv > 5) {
                        char *p;
                        p = strchr(buf, '\n');
                        if (p && !strncmp(buf, "SIZE:", 5)) {
                                *p = '\0';
                                fsize = atoi(buf + 5);
                                fp = p + 1;
                                nrecv -= strlen(buf) + 1;
                                first = 0;
                        }
                }
                
                write(fd, fp, nrecv);
                fsize -= nrecv;

                if (!fsize)
                        break;
        }

        close(fd);

        if (nrecv < 0) {
                fprintf(stderr, "recv from server error\n");
                close(clientfd);
                exit(1);
        }

        if (system("tar -xzf files.tar.gz -C .") < 0) {
                fprintf(stderr, "tar failed!\n");
                close(clientfd);
                exit(1);
        }

        printf("All files received!\n");

        unlink("files.tar.gz");
        close(clientfd);
}



/**
 * @brief Create a new listen socket and start listening for connections.
 * 
 * @param port : port number for the server to connect
 */
static int open_listenfd(char *port)
{
        int listenfd, err;
        struct addrinfo *p, *listp;
        struct addrinfo hints;

        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_socktype = SOCK_STREAM;                /* TCP connection */
        hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;    /* Using any IP addr */
        hints.ai_flags |= AI_NUMERICSERV;               /* Using port number */

        if ((err = getaddrinfo(NULL, port, &hints, &listp)) != 0) {
                fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(err));
                exit(1);
        }

        /* Walk the list for one that we can successfully connect to */
        for (p = listp; p; p = p->ai_next) {
                if ((listenfd = server_init(p->ai_socktype, p->ai_addr, p->ai_addrlen, QLEN)) >= 0)
                        break;          /* Success */
        }

        freeaddrinfo(listp);

        if (!p) /* All connects failed */
                return -1;

        return listenfd;
        
}


/**
 * @brief Initialize the server.
 * 
 * @param type : type of the socket (SOCK_STREAM for TCP protocol)
 * @param addr : socket address for the server
 * @param alen : size of addr
 * @param backlog : number of outstanding connect requests that can be enqueued
 * @return int : If the server set up succeeds, zero is returned. 
 * On error, -1 is returned, and errno is set appropriately.
 */
static int server_init(int type, const struct sockaddr *addr, socklen_t alen, int backlog)
{
        int sockfd;
        int err = 0;
        int reuse = 1;

        if ((sockfd = socket(addr->sa_family, type, 0)) < 0)
                return -1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int)) < 0)
                goto errout;
        if (bind(sockfd, addr, alen) < 0)
                goto errout;
        if (type == SOCK_STREAM || type == SOCK_SEQPACKET) {
                if (listen(sockfd, backlog) < 0)
                        goto errout;
        }

        return sockfd;

errout:
        err = errno;
        close(sockfd);
        errno = err;
        return -1;
}


static void processclient(int listenfd)
{
        pid_t pid;
        int connfd;
        socklen_t clientlen;
        struct sockaddr_storage clientaddr;     /* Enough room for any addresses */
        char client_hostname[MAXLINE], client_port[MAXLINE];
        
        /* close-on-exec: child processes will close this fd automatically */
        if (set_cloexec(listenfd) < 0)
                fprintf(stderr, "Close-on-exec failed!\n");

        while (1) {

                /* connect to a new client */
                clientlen = sizeof(struct sockaddr_storage);
                if ((connfd = accept(listenfd, (struct sockaddr*)&clientaddr, &clientlen)) < 0) 
                        fprintf(stderr, "Connection failed! Error at accept.\n");
                
                /* print the new connection message */
                getnameinfo((struct sockaddr*)&clientaddr, clientlen, client_hostname, MAXLINE, client_port, MAXLINE, 0);
                
                fprintf(stdout, "-----------------------------------------------------\n");
                
                fprintf(stdout, "Connected to (%s, %s)\n", client_hostname, client_port);

                socketfd.nclient++;

                /* fork a new child for this client*/
                if ((pid = fork()) < 0) {
                        fprintf(stderr, "fork error\n");
                        exit(1);
                } else if (pid == 0) {
                        while(1) process(connfd);
                
                }
        
                close(connfd);
        }
}


static int set_cloexec(int fd)
{
	int val;

	if ((val = fcntl(fd, F_GETFD, 0)) < 0)
	        return -1;

	val |= FD_CLOEXEC;		/* enable close-on-exec */

	return fcntl(fd, F_SETFD, val);
}


/* signal handler for sigchld to reap all zombie children */
static void sigchld_handler(int signum) {
        while(waitpid(-1, 0, WNOHANG) > 0) {
                fprintf(stderr, "Child exited\n");
        }
}


static void process(int connfd) {
        int clientfd;
        int nrecv = 0;
        int nbuf = 0;
        char buf[MAXLINE];
        char EOM = '\n';

        memset(buf, 0, MAXLINE);

        while ((nrecv = recv(connfd, buf + nbuf, MAXLINE - nbuf, 0)) > 0) {
                nbuf += nrecv;
                if (buf[nbuf-1] == EOM)
                        break;
        }

        buf[nbuf] = '\0';

        if (nrecv < 0) {
                fprintf(stderr, "recv from client error\n");
                exit(1);
        }

        fprintf(stdout, "The command from child is: %s", buf);

        /* have got the full command here */
        eval(buf, nbuf);
        switch (status) {
                case OK:    
                        send_text(message, connfd);
                        break;
                case ERR:
                        send_text(message, connfd);   
                        break;
                case FILE:
                        int fd = open("temp.tar.gz", O_RDONLY);
                        send_file(fd, connfd);
                        close(fd);
                        unlink("temp.tar.gz");
                        break;
                case QUIT:
                        close(connfd);
                        exit(0);
        }

}


static int parse(char *buf, char *argv[MAXARG])
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


int findfile(const char *fpath, const struct stat *st, int type)
{
        switch (type) {
        case FTW_NS:
                fprintf(stderr, "findfile failed!\n");
                break;
        case FTW_F:
                char *fname = basename((char*)fpath);
                if (!strcmp(fname, extr_arg[1])) {
                        sprintf(message, "OK:%s, %lld, %s", fname, (long long) st->st_size, ctime(&st->st_ctime));
                        return 1;
                }
        default:
                break;
        }
        return 0;
}


int sdgetfiles(const char *fpath, const struct stat *st, int type)
{
        switch (type) {
        case FTW_NS:
                fprintf(stderr, "sgetfiles failed!\n");
                break;
        case FTW_F:
                if (compare(st, (void*)extr_arg[1], (void*)extr_arg[2], extr_arg[0])) {
                        strcat(message, " ");
                        strcat(message, fpath);
                        status = OK;
                }
                break;
        default:
                break;
        }
        return 0;
}


int getfiles(const char *fpath, const struct stat *st, int type)
{
        switch (type) {
        case FTW_NS:
                fprintf(stderr, "sgetfiles failed!\n");
                break;
        case FTW_F:
                char *fname = basename((char*)fpath);

                if (contains(extr_arg + 1, fname)) {
                        strcat(message, " ");
                        strcat(message, fpath);
                        status = OK;
                }
                break;
        default:
                break;
        }
        return 0;
}

int gettargz(const char *fpath, const struct stat *st, int type)
{
        switch (type) {
        case FTW_NS:
                fprintf(stderr, "sgetfiles failed!\n");
                break;
        case FTW_F:
                if (match(extr_arg + 1, fpath)) {
                        strcat(message, " ");
                        strcat(message, fpath);
                        status = OK;
                }
                break;
        default:
                break;
        }
        return 0;
}


static int eval(char *msg, int size) {
        int i, argc;
        char *argv[MAXARG];

        if ((argc = parse(msg, argv)) < 0) {
                fprintf(stderr, "parse from the server: command not found.\n");
                exit(1);
        }

        status = ERR;

        strcpy(message, "tar -czf temp.tar.gz");

        extr_arg = argv;
        
        /* check first argument */
        if (!strcmp(*argv, "findfile")) {
                if (ftw(PATH, findfile, 20)) status = OK;
                else {
                        status = ERR;
                        strcpy(message, "ERR:File not found");
                }
        
        } else if (!strcmp(*argv, "sgetfiles") || !strcmp(*argv, "dgetfiles")) {
                ftw(PATH, sdgetfiles, 20);
                if (status == OK) {
                        system(message);
                }
                status = FILE;

        } else if (!strcmp(*argv, "getfiles")) {
                ftw(PATH, getfiles, 20);
                if (status == OK) {
                        system(message);
                        status = FILE;
                } else {
                        status = ERR;
                        strcpy(message, "ERR:No file found");
                }

                printf("%s\n", message);

        } else if (!strcmp(*argv, "gettargz")) {
                ftw(PATH, gettargz, 20);
                if (status == OK) {
                        system(message);
                        status = FILE;

                } else {
                        status = ERR;
                        strcpy(message, "ERR:No file found");
                }
        } else if (!strcmp(*argv, "quit")) {
                status = QUIT;
        } else {
                fprintf(stderr, "eval from the server: command not found.\n");
                status = ERR;
        }

}


static void send_file(int fd, int connfd)
{       
        struct stat stat_buf;

        fstat(fd, &stat_buf);

        int nsend;
        printf("%d\n", stat_buf.st_size);
        
        char size[MAXLINE];

        sprintf(size, "SIZE:%d\n", stat_buf.st_size);
        if ((nsend = send(connfd, size, strlen(size), 0)) < 0) {
                close(connfd);
                fprintf(stderr, "sendfile failed!\n");
                exit(1);
        }    

        /* send files.tar.gz to the mirror */
        if ((nsend = sendfile(connfd, fd, NULL, stat_buf.st_size)) < 0) {
                close(connfd);
                fprintf(stderr, "sendfile failed!\n");
                exit(1);
        }
        
}


static void send_text(char *msg, int connfd) 
{
        if (send(connfd, msg, strlen(msg) + 1, 0) < 0) {
                fprintf(stderr, "send failed!\n");
                close(connfd);
                exit(1);
        }
}


static int compare(const struct stat *st, void *c1, void *c2, char *type)
{
        if (!strcmp(type, "sgetfiles")) {
                int size = st->st_size;
                return size >= atoi((char*) c1) && size <= atoi((char *)c2);
        } else {
                int ct = st->st_ctime;
                struct tm tm1, tm2;
                int year;
                sscanf((char*)c1, "%*s %*s %*2d %*02d:%*02d:%*02d %d", &year);
                tm1.tm_year = year - 1900;
                sscanf((char*)c1, "%*s %*s %*2d %*02d:%*02d:%*02d %*d", &tm1.tm_mon, &tm1.tm_mday, &tm1.tm_hour, &tm1.tm_min, &tm1.tm_sec);
                tm1.tm_mon--;
                tm1.tm_isdst = -1;
                time_t ctime_t1 = mktime(&tm1);

                sscanf((char*)c2, "%*s %*s %*2d %*02d:%*02d:%*02d %d", &year);
                tm1.tm_year = year - 1900;
                sscanf((char*)c2, "%*s %*s %*2d %*02d:%*02d:%*02d %*d", &tm1.tm_mon, &tm1.tm_mday, &tm1.tm_hour, &tm1.tm_min, &tm1.tm_sec);
                tm1.tm_mon--;
                tm1.tm_isdst = -1;
                time_t ctime_t2 = mktime(&tm2);

                return ct >= ctime_t1 && ct <= ctime_t2;
        }
}


static int contains(char *args[], char *fname)
{
        for (int i = 0; args[i]; ++i) {
                if (!strcmp(fname, args[i]))
                        return 1;
        }

        return 0;
}


static int get_file_ext(const char *fname, char *ext) 
{
        char *dot = strrchr(fname, '.');
        if (dot && dot != fname) {
                strcpy(ext, dot + 1);
                return 1;
        } 

        return 0;
}

static int match(char *args[], const char *fname)
{
        char ext[MAXLINE];

        if (!get_file_ext(fname, ext)) return 0;

        for (int i = 0; args[i]; ++i) {
                printf("%s %s\n", ext, args[i]);
                if (!strcmp(ext, args[i])) 
                        return 1;
        }

        return 0;
}
