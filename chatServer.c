#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
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

int main(int argc, char *argv[]) {
    int port = checkForErrors(argc, argv);
    signal(SIGINT, intHandler);

    conn_pool_t *pool = malloc(sizeof(conn_pool_t));
    init_pool(pool);
    char buffer[BUFFER_SIZE];

    /*************************************************************/
    /* Create an AF_INET stream socket to receive incoming      */
    /* connections on                                            */
    /*************************************************************/
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
    listen(mainSD, 32);
    FD_SET(mainSD, &pool->read_set);
    FD_SET(mainSD, &pool->write_set);

    /*************************************************************/
    /* Initialize fd_sets  			                             */
    /*************************************************************/

    /*************************************************************/
    /* Loop waiting for incoming connects, for incoming data or  */
    /* to write data, on any of the connected sockets.           */
    /*************************************************************/
    do {
        if(pool->maxfd == 0) {
            pool->maxfd = mainSD;
        }
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
        /**********************************************************/
        /* One or more descriptors are readable or writable.      */
        /* Need to determine which ones they are.                 */
        /**********************************************************/
        /* check all descriptors, stop when checked all valid fds */
        for (int i = mainSD; i <= pool->maxfd && pool->nready > 0; i++) {
            /* Each time a ready descriptor is found, one less has  */
            /* to be looked for.  This is being done so that we     */
            /* can stop looking at the working set once we have     */
            /* found all of the descriptors that were ready         */

            /*******************************************************/
            /* Check to see if this descriptor is ready for read   */
            /*******************************************************/
            if (FD_ISSET(i, &pool->ready_read_set)) {
                pool->nready--;

                /***************************************************/
                /* A descriptor was found that was readable		   */
                /* if this is the listening socket, accept one      */
                /* incoming connection that is queued up on the     */
                /*  listening socket before we loop back and call   */
                /* select again. 						            */
                /****************************************************/
                if (i == mainSD) {
                    int newSD = accept(mainSD, NULL, NULL);
                    if (newSD < 0)
                        continue;
                    printf("New incoming connection on sd %d\n", i);
                    add_conn(newSD, pool);
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

                /****************************************************/
                /* If this is not the listening socket, an 			*/
                /* existing connection must be readable				*/
                /* Receive incoming data his socket             */
                /****************************************************/

                /* If the connection has been closed by client 		*/
                /* remove the connection (remove_conn(...))    		*/

                /**********************************************/
                /* Data was received, add msg to all other    */
                /* connectios					  			  */
                /**********************************************/

            } /* End of if (FD_ISSET()) */
            /*******************************************************/
            /* Check to see if this descriptor is ready for write  */
            /*******************************************************/
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
    return 0;
}


int init_pool(conn_pool_t *pool) {
    //initialized all fields
    pool->maxfd = 0;
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
    while (cur != NULL) {
        if (cur->fd == sd) {
            // remove connection from pool
            if (cur->prev != NULL) {
                cur->prev->next = cur->next;
            }
            if (cur->next != NULL) {
                cur->next->prev = cur->prev;
            }
            if (cur == pool->conn_head) {
                pool->conn_head = cur->next;
            }
            pool->nr_conns--;
            free(cur);
            FD_CLR(sd, &pool->read_set);
            FD_CLR(sd, &pool->write_set);

            if (sd == pool->maxfd) {
                int max = 0;
                conn_t *temp = pool->conn_head;
                while (temp != NULL) {
                    if (temp->fd > max) {
                        max = temp->fd;
                    }
                    temp = temp->next;
                }
                pool->maxfd = max;
            }
            printf("removing connection with sd %d \n", sd);
            return SUCCESS;
        }
        cur = cur->next;
    }
    return ERROR;
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
            msg->prev = NULL;
            if (cur->write_msg_head == NULL) {
                cur->write_msg_head = msg;
                cur->write_msg_tail = msg;
            } else {
                cur->write_msg_tail->next = msg;
                msg->prev = cur->write_msg_tail;
                cur->write_msg_tail = msg;
            }
            FD_SET(cur->fd, &pool->write_set);
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
            return SUCCESS;
        }
        cur = cur->next;
    }
    return ERROR;
}
