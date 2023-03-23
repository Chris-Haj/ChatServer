#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <netinet/in.h>
#include "chatServer.h"

#define SUCCESS 0
#define ERROR (-1)

static int end_server = 0;

void intHandler(int SIG_INT) {
    /* use a flag to end_server to break the main loop */
    end_server = 1;
}

void UsageError() {
    printf("Usage: server <port>");
    exit(EXIT_FAILURE);
}

int checkForErrors(int argc, char *argv[]) {
    if (argc != 2)
        UsageError();
    int port = atoi(argv[1]);
    if (port < 1 || port > 65535)
        UsageError();
    return port;
}

void removeAllConnectionsLeft(conn_pool_t *pool) {

    conn_t *cur = pool->conn_head;
    while (cur != NULL) {
        conn_t *next = cur->next;
        free(cur);
        cur = next;
    }

}

int main(int argc, char *argv[]) {
    int port = checkForErrors(argc, argv);
    signal(SIGINT, intHandler);

    conn_pool_t *pool = malloc(sizeof(conn_pool_t));
    init_pool(pool);
    char buffer[BUFFER_SIZE];

    int mainSD = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    /*************************************************************/
    /* Set socket to be nonblocking. All of the sockets for      */
    /* the incoming connections will also be nonblocking since   */
    /* they will inherit that state from the listening socket.   */
    /*************************************************************/
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    ioctl(mainSD, FIONBIO, (char *) &on);
    if (0 > bind(mainSD, (struct sockaddr *) &server_addr, sizeof(server_addr))) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    /*************************************************************/
    /* Set the listen back log                                   */
    /*************************************************************/
    listen(mainSD, 5);
    FD_SET(mainSD, &pool->read_set);
    FD_SET(mainSD, &pool->write_set);
    pool->maxfd = mainSD;

    /*************************************************************/
    /* Initialize fd_sets  			                             */
    /*************************************************************/

    /*************************************************************/
    /* Loop waiting for incoming connects, for incoming data or  */
    /* to write data, on any of the connected sockets.           */
    /*************************************************************/
    do {
        /**********************************************************/
        /* Copy the master fd_set over to the working fd_set.     */
        /**********************************************************/
        pool->ready_read_set = pool->read_set;
        pool->ready_write_set = pool->write_set;

        printf("Waiting on select()...\nMaxFd %d\n", pool->maxfd);
        /**********************************************************/
        /* Call select() and get next fd 										  */
        /**********************************************************/
        pool->nready = select(pool->maxfd + 1, &pool->ready_read_set, &pool->ready_write_set, NULL, NULL);
        if (pool->nready < 0) {
            perror("select");
            //free all memory
            exit(EXIT_FAILURE);
        }

        for (int i = mainSD; i <= pool->maxfd ; i++) {

            if (FD_ISSET(i, &pool->ready_read_set)) {

                if (i == mainSD) {
                    int newSD = accept(mainSD, NULL, NULL);
                    if (newSD < 0)
                        continue;
                    printf("New incoming connection on sd %d\n", i);
                    add_conn(newSD, pool);
                    break;
                } else {
                    /***************************************************/
                    /* This is not the listening socket, therefore an  */
                    /* existing connection must be readable.           */
                    /***************************************************/
                    printf("Descriptor %d is readable\n", i);
                    size_t length = read(i, buffer, BUFFER_SIZE);
                    printf("%lu bytes read from %d\n", length, i);
                    if (length == 0) {
                        printf("Connection closed for sd %d\n", i);
                        printf("removing connection with sd %d\n", i);
                        remove_conn(i, pool);
                        break;
                    } else {
                        add_msg(i, buffer, (int) length, pool);
                    }
                }

            }
            if (FD_ISSET(i, &pool->ready_write_set)) {
                /* try to write all msgs in queue to sd */
                write_to_client(i, pool);
            }
            /*******************************************************/


        } /* End of loop through selectable descriptors */

    } while (end_server == 0);

    /*************************************************************/
    /* If we are here, Control-C was typed,						 */
    /* clean up all open connections					         */
    /*************************************************************/
    removeAllConnectionsLeft(pool);
    free(pool);
    return 0;
}


int init_pool(conn_pool_t *pool) {
    //initialized all fields
    pool->maxfd = 3;
    pool->nready = 0;
    pool->nr_conns = 0;
    FD_ZERO(&pool->read_set);
    FD_ZERO(&pool->ready_read_set);
    FD_ZERO(&pool->write_set);
    FD_ZERO(&pool->ready_write_set);
    pool->conn_head = NULL;
    return SUCCESS;
}

int add_conn(int sd, conn_pool_t *pool) {
    conn_t *conn = malloc(sizeof(conn_t));
    if (conn == NULL)
        return ERROR;
    pool->nr_conns++;
    conn->fd = sd;
    conn->next = NULL;
    conn->prev = NULL;
    conn->write_msg_head = NULL;
    conn->write_msg_tail = NULL;

    if (pool->conn_head == NULL) {
        pool->conn_head = conn;
        return SUCCESS;
    }
    conn_t *cur = pool->conn_head;
    while (cur->next != NULL) {
        cur = cur->next;
    }
    cur->next = conn;
    conn->prev = cur;
    if (sd > pool->maxfd)
        pool->maxfd = sd;
    FD_SET(sd, &pool->read_set);
    FD_SET(sd, &pool->write_set);
    return SUCCESS;
}


int remove_conn(int sd, conn_pool_t *pool) {
    /*
    * 1. remove connection from pool
    * 2. deallocate connection
    * 3. remove from sets
    * 4. update max_fd if needed
    */
    conn_t *cur = pool->conn_head;
    conn_t *Prev = NULL;
    while (cur != NULL) {
        if (cur->fd == sd) {
            Prev = cur->prev;
            if (cur->next != NULL && cur->prev != NULL) {
                conn_t *next = cur->next;
                next->prev = Prev;
                Prev->next = cur;
            } else if (cur->next != NULL) {
                conn_t *next = cur->next;
                next->prev = NULL;
                pool->conn_head = cur->next;
            } else if (cur->prev != NULL) {
                Prev->next = NULL;
            }
            else
                pool->conn_head= NULL;
            if (cur->write_msg_head != NULL) {
                msg_t *msg = cur->write_msg_head;
                while (msg != NULL) {
                    msg_t *next = msg->next;
                    msg_t *prev = msg->prev;
                    if (next == NULL){
                        free(msg);
                        break;
                    }
                    if (prev != NULL)
                        free(prev);
                    if (msg->message != NULL)
                        free(msg->message);

                    msg = next;
                }
            }
        }
        cur = cur->next;
    }
    FD_CLR(sd, &(pool->read_set));
    FD_CLR(sd, &(pool->write_set));
    FD_CLR(sd, &(pool->ready_read_set));
    FD_CLR(sd, &(pool->ready_write_set));
    pool->nr_conns--;
    if (sd >= pool->maxfd) {
        pool->maxfd = 3;
        if (pool->nr_conns > 0) {
            cur = pool->conn_head;
            while (cur != NULL) {
                if (cur->fd > pool->maxfd)
                    pool->maxfd = cur->fd;
                cur = cur->next;
            }
        }
    }
    close(sd);
    free(cur);
    return SUCCESS;
}

int add_msg(int sd, char *buffer, int len, conn_pool_t *pool) {

    /*
     * 1. add msg_t to write queue of all other connections
     * 2. set each fd to check if ready to write`
     */

    conn_t *cur = pool->conn_head;
    while (cur != NULL) {
        if (cur->fd != sd) {
            msg_t *msg = malloc(sizeof(msg_t));
            msg->message = malloc(len + 1);
            memcpy(msg->message, buffer, len);
            msg->message[len] = '\0';
            msg->size = len;
            msg->next = NULL;
            msg->prev = cur->write_msg_tail;
            cur->write_msg_tail = msg;
            FD_SET(cur->fd, &pool->write_set);
            if(cur->write_msg_head == NULL){
                cur->write_msg_head = NULL;
            }
        }
        cur = cur->next;
    }
    return SUCCESS;
}

int write_to_client(int sd, conn_pool_t *pool) {

    /*
     * 1. write all msgs in queue
     * 2. deallocate each writen msg
     * 3. if all msgs were writen successfully, there is nothing else to write to this fd... */

    conn_t *cur = pool->conn_head;
    while (cur != NULL) {
        if (cur->fd == sd) {
            msg_t *msg = cur->write_msg_head;
            while (msg != NULL) {
                if (write(sd, msg->message, msg->size) < 0) {
                    return ERROR;
                }
                msg_t *temp = msg;
                msg = msg->next;
                free(temp->message);
                free(temp);

            }
            cur->write_msg_head = NULL;
            cur->write_msg_tail = NULL;
            FD_CLR(sd, &pool->write_set);
            FD_CLR(sd, &pool->ready_write_set);
            return SUCCESS;
        }
        cur = cur->next;
    }
    return ERROR;
}
