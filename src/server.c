/******************************************************************************
* Authors: Lincong Li                                                         *
*******************************************************************************/

#include "utility.h"
#include "connection_handlers.h"
#include "logger.h"
#include "message.h"
#include "http.h"
#include "file_handlers.h"
#define USAGE "\nLiso Server Usage: ./lisod <HTTP port> <log file> <www folder>\n"

int http_port;
int https_port;
char *WWW_DIR;
char *CGI_scripts;
int http_sock_fd; // a file descriptor for our "listening" socket.
peer_t connection_list[MAX_CLIENTS];

int daemonize(char* lock_file);
void server_shutdown_properly(int code);
void handle_signal_action(int sig_number);
int setup_signals();
int build_fd_sets(int listen_sock, fd_set *read_fds, fd_set *write_fds, fd_set *except_fds);
void init_server(int argc, char* argv[]); // set http port and other parameters

int main(int argc, char* argv[])
{
    init_server(argc, argv);
    SERVER_LOG("%s", "Setting up signal handlers...")

    if (start_listen_socket(BACKLOG, SOCK_REUSE, &http_sock_fd) != 0)
        exit(EXIT_FAILURE);

    SERVER_LOG("%s", "Initializing peers...")
    int i;
    for (i = 0; i < MAX_CLIENTS; ++i) {
        connection_list[i].socket = NO_SOCKET;
        create_peer(&connection_list[i]);
    }
    SERVER_LOG("%s", "Done")

    fd_set read_fds;
    fd_set write_fds;
    fd_set except_fds;
    struct timeval timeout;
//    timeout.tv_sec = 0;

    int high_sock = http_sock_fd;

    bool has_read;
    bool has_sent;
    peer_t* curr_client;
    int cgi_fd;
    // loop waiting for input and then write it back
    while (1)
    {
//        SERVER_LOG("%s", "Trying to build fd_set")
        build_fd_sets(http_sock_fd, &read_fds, &write_fds, &except_fds);
//        SERVER_LOG("%s", "Done building fd_set")
        high_sock = http_sock_fd;

        // get the highest file descriptor
        for (i = 0; i < MAX_CLIENTS; ++i) {
            if (connection_list[i].socket > high_sock)
                high_sock = connection_list[i].socket;

            if (connection_list[i].cgi_executor != NULL) {
                high_sock = MAX(connection_list[i].cgi_executor->stdin_pipe[1], high_sock);
                high_sock = MAX(connection_list[i].cgi_executor->stdout_pipe[0], high_sock);
            }
        }
//        SERVER_LOG("%s", "Selecting...")
        int activity = select(high_sock + 1, &read_fds, &write_fds, &except_fds, NULL); // &timeout); //  NULL);
//        SERVER_LOG("%s", "Done selecting")

        switch (activity) {
            case -1:
                perror("select()");
                SERVER_LOG("%s", "select() returns -1 error. Shutting down the server...")
                server_shutdown_properly(EXIT_FAILURE);

            case 0: // timeout
                SERVER_LOG("%s", "select() returns 0.")
                fprintf(stderr, "select() returns 0.\n");

            default:

                if (FD_ISSET(http_sock_fd, &read_fds)) {
                    handle_new_connection(http_sock_fd, connection_list);
                }

                if (FD_ISSET(STDIN_FILENO, &except_fds)) {
                    SERVER_LOG("%s", "except_fds for stdin.")
                    server_shutdown_properly(EXIT_FAILURE);
                }

                if (FD_ISSET(http_sock_fd, &except_fds)) {
                    SERVER_LOG("%s", "Exception listen socket fd.")
                    server_shutdown_properly(EXIT_FAILURE);
                }

                for (i = 0; i < MAX_CLIENTS; ++i) {
                    has_read = false;
                    has_sent = false;
                    curr_client = &(connection_list[i]);

                    if (curr_client->cgi_executor != NULL) {
                        cgi_fd = curr_client->cgi_executor->stdin_pipe[1];
                        // client can send data from its CGI buffer to its child CGI process
                        int ret;
                        if (FD_ISSET(cgi_fd, &write_fds)) {
//                            SERVER_LOG("%s", "Try handle send_to_CGI_process()")
                            ret = send_to_CGI_process(curr_client, cgi_fd);
                            if (ret == EXIT_FAILURE) {
                                SERVER_LOG("%s", "send_to_CGI_process failed. closing connection...")
                                close_client_connection(curr_client);
                            }
                        }

                        cgi_fd = curr_client->cgi_executor->stdout_pipe[0];
                        // client can receive data from its child CGI process and write it to its sending buffer
                        if (FD_ISSET(cgi_fd, &read_fds)) {
                            SERVER_LOG("%s", "Try handle pipe_from_CGI_process_to_client()")
                            ret = pipe_from_CGI_process_to_client(curr_client, cgi_fd);
                            if (ret == EXIT_FAILURE) {
                                SERVER_LOG("%s", "pipe_from_CGI_process_to_client failed. closing connection...")
                                close_client_connection(curr_client);
                            }
                        }
                    }

                    // there are date from the TCP port for this client to read into its receiving buffer
                    if (curr_client->socket != NO_SOCKET) {
                        if (FD_ISSET(curr_client->socket, &read_fds)) {
                            if (receive_from_peer(curr_client, &echo_received_message) == EXIT_SUCCESS) {
                                has_read = true;

                            } else {
                                close_client_connection(curr_client);
                                continue;
                            }
                        }

                        // even thought there is no incoming data for this client to read, we can still check if there
                        // is anything already in the receiving_buffer
                        if(!buf_empty(&curr_client->receiving_buffer))
                            has_read = true;
                    }

                    // the TCP port is ready to send data out from the sending buffer of the client
                    if (curr_client->socket != NO_SOCKET &&
                        FD_ISSET(curr_client->socket, &write_fds)){ //&& !buf_empty(&connection_list[i].sending_buffer) )) {
                        int ret = send_to_peer(curr_client); // sending bytes in the sending buffer to clients
                        if (ret == EXIT_FAILURE) {
                            close_client_connection(curr_client);
                            continue;

                        } else if(ret == EXIT_SUCCESS) {
                            SERVER_LOG("%s", "send_to_peer returns EXIT_SUCCESS!")
                            has_sent = true;
                        } // otherwise if ret == NOTHING_TO_SEND, do nothing
                    }

                    if (connection_list[i].socket != NO_SOCKET &&
                        FD_ISSET(curr_client->socket, &except_fds)) {
                        SERVER_LOG("%s", "Exception client fd.")
                        close_client_connection(curr_client);
                        continue;
                    }

                    // handle HTTP
                    if(has_read || has_sent) {
                        int ret = handle_http(curr_client); // actually handle HTTP
                        if(ret == CLOSE_CONN_IMMEDIATELY){
                            SERVER_LOG("Close connection %d", i)
                            close_client_connection(curr_client);

                        } else if(ret == CLOSE_CONN) {
                            curr_client->close_conn = true;
                            SERVER_LOG("Keep connection %d. But this is the last connection", i)

                        } else {
//                            SERVER_LOG("Keep connection %d", i)
                        }
                    }
                }
        }
    }
    return EXIT_SUCCESS;
}

void server_shutdown_properly(int code)
{
    int i;

    close(http_sock_fd);

    for (i = 0; i < MAX_CLIENTS; ++i) {
        if (connection_list[i].socket != NO_SOCKET) {
            close_client_connection(&connection_list[i]);
        }
    }
    printf("Shutdown server properly.\n");
    exit(code);
}

int build_fd_sets(int listen_sock, fd_set *read_fds, fd_set *write_fds, fd_set *except_fds)
{
    int i;
    FD_ZERO(read_fds);
    FD_SET(listen_sock, read_fds);
    FD_ZERO(write_fds);
    FD_ZERO(except_fds);
    FD_SET(listen_sock, except_fds);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (connection_list[i].socket != NO_SOCKET) {
            FD_SET(connection_list[i].socket, read_fds);
            FD_SET(connection_list[i].socket, write_fds);
            if (connection_list[i].cgi_executor != NULL) {
                FD_SET(connection_list[i].cgi_executor->stdin_pipe[1], write_fds);
                FD_SET(connection_list[i].cgi_executor->stdout_pipe[0], read_fds);
            }
        }
    }

    return 0;
}

void handle_signal_action(int sig_number)
{
    if (sig_number == SIGINT) {
        printf("SIGINT was catched!\n");
        server_shutdown_properly(EXIT_SUCCESS);
    }
    else if (sig_number == SIGPIPE) { // there is nobody to receive your data
        printf("SIGPIPE was catched!\n");
         server_shutdown_properly(EXIT_SUCCESS);
    }
}

int setup_signals()
{
    signal(SIGINT, handle_signal_action);
    signal(SIGTERM, handle_signal_action);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    return 0;
}

void init_server(int argc, char* argv[])
{
    /* Check usage */
    char* log_file;
    char *lock_file;

    if (argc < 8) {
        fprintf(stdout, USAGE);
        http_port = 9999;
        WWW_DIR = DEFAULT_WWW_DIR;
        log_file = LOG_FILE_NAME;
    } else {
        /* Read parameters from command line arguments */
        http_port = atoi(argv[1]);   /* port param */
        https_port = atoi(argv[2]); /* https port param */
        log_file = argv[3];          /* log file param */
        lock_file = argv[4];         /* lock file param */
        WWW_DIR = argv[5];       /* www folder param */
        CGI_scripts = argv[6];      /* cgi script param */
        // PRIVATE_KEY_FILE = argv[7]; /* private key file param */
        // CERT_FILE = argv[8];        /* certificate file param */
    }
    // check if resources folder exists
    if(!is_dir(WWW_DIR)){
        HTTP_LOG("%s", "WWW path is wrong")
        exit(EXIT_FAILURE);
    }

    printf("Trying to open log_file: %s\n", log_file);

    if (LOG_TO_FILE)
        log_fd = fopen(log_file, "a+");
    else
        log_fd = stdout;

    if(log_fd == NULL){
        HTTP_LOG("%s", "Open log file failed")
        exit(EXIT_FAILURE);
    }
    if(RUN_AS_DAEMON) {
        fprintf(stdout, "Run server as a daemon process");
        if (daemonize(lock_file) != EXIT_SUCCESS) {
            fprintf(stderr, "Daemonizing Lisod fails");
            exit(EXIT_FAILURE);
        }

    } else {
        if (setup_signals() != 0)
            exit(EXIT_FAILURE);
        SERVER_LOG("%s", "Done")
    }
}

int daemonize(char* lock_file) {
    if(strlen(lock_file) == 0) {
        fprintf(stdout, "Illegal lock file name\n");
        return EXIT_FAILURE;
    }

    int i, lfp, pid = fork();
    char str[256] = {0};
    if (pid < 0) exit(EXIT_FAILURE);
    if (pid > 0) exit(EXIT_SUCCESS);

    setsid();

    for (i = getdtablesize(); i>=0; i--)
        close(i);

    i = open("/dev/null", O_RDWR);
    dup(i); /* stdout */
    dup(i); /* stderr */
    umask(027);
    printf("Trying to open %s\n", lock_file);
    lfp = open(lock_file, O_RDWR|O_CREAT, 0640);

    if (lfp < 0)
        exit(EXIT_FAILURE); /* can not open */

    if (lockf(lfp, F_TLOCK, 0) < 0)
        exit(EXIT_SUCCESS); /* can not lock */

    /* only first instance continues */
    sprintf(str, "%d\n", getpid());
    write(lfp, str, strlen(str)); /* record pid to lockfile */
    close(lfp);
    signal(SIGINT, handle_signal_action);
    signal(SIGTERM, handle_signal_action);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGCHLD, SIG_IGN); /* child terminate signal */

    return EXIT_SUCCESS;
}
