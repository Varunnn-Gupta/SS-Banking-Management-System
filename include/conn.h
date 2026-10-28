#ifndef CONN_H
#define CONN_H
void handle_connection(int fd);   /* runs in the client's thread until it disconnects */
#endif
