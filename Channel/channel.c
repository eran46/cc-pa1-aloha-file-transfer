#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <time.h>
#include <conio.h>

#pragma comment(lib, "Ws2_32.lib")

#define MAX_CLIENTS 10
#define BUFFER_SIZE 2048
#define COLLISION_SIGNAL "NOISE"   // A special signal (should be distinguishable from valid frames)
#define COLLISION_SIGNAL_LEN 5

// Structure to hold client (server) info and statistics.
typedef struct {
    SOCKET sock;
    struct sockaddr_in addr;
    int frames_received;
    int collisions;
    int total_bytes;  /* Total bytes* received successfully with out collision* from this client. */
} Client;

typedef struct {
    struct sockaddr_in addr;
    int frames_received;
    int collisions;
    int total_bytes;
} ClientStats;


// Function to remove a client (close its socket and shift the following clients backwards in the array)
// and copy the client stats to allstats array
// Returns client count
int removeClient(Client clients[], int count, int index, ClientStats all_stats[], int* stats_count_ptr) {
    // Save stats before removing
    all_stats[*stats_count_ptr].addr = clients[index].addr;
    all_stats[*stats_count_ptr].frames_received = clients[index].frames_received;
    all_stats[*stats_count_ptr].collisions = clients[index].collisions;
    all_stats[*stats_count_ptr].total_bytes = clients[index].total_bytes;
    (*stats_count_ptr)++;

    closesocket(clients[index].sock);

    // Shift remaining clients left
    for (int j = index; j < count - 1; j++) {
        clients[j] = clients[j + 1];
    }

    return count - 1;
}


int main(int argc, char* argv[]) {

    if (argc != 3) { // more or less than 2 commandline arguments
        fprintf(stderr, "Usage: %s <chan_port> <slot_time_ms>\n", argv[0]);
        return 1;
    }

    int chan_port = atoi(argv[1]);   // channel port number
    int slot_time = atoi(argv[2]);   // slot time in milliseconds

    // ----- Initialize Windows networking ----->
    WSADATA wsaData;
    int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (iResult != NO_ERROR) {
        fprintf(stderr, "Error at WSAStartup()\n");
        return 1;
    }

    struct sockaddr_in channel_addr; // IPv4, unspecified machine (channel) ip, port from argument
    channel_addr.sin_family = AF_INET;
    channel_addr.sin_addr.s_addr = INADDR_ANY;
    channel_addr.sin_port = htons(chan_port); // convert host to network endiannes, port from commandline argument

    // Create the channel's listening socket - the interface
    SOCKET channel_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (channel_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed.\n");
        WSACleanup();
        return 1;
    }

    // Bind the channel's socket to the specified port and allowed incoming address via channel_addr
    if (bind(channel_sock, (SOCKADDR*)&channel_addr, sizeof(channel_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "bind() failed.\n");
        closesocket(channel_sock);
        WSACleanup();
        return 1;
    }

    // Designate as Passive Participant socket in TCP communications (acc. to rec 1)
    if (listen(channel_sock, MAX_CLIENTS) == SOCKET_ERROR) { // MAX_CLIENTS maximum connection-queue length
        fprintf(stderr, "listen() failed.\n");
        closesocket(channel_sock);
        WSACleanup();
        return 1;
    }

    // channel_sock in non-blocking mode
    u_long mode = 1;
    ioctlsocket(channel_sock, FIONBIO, &mode);

    // Array for client info
    Client clients[MAX_CLIENTS]; // each client is a server
    int client_count = 0;

    ClientStats all_stats[MAX_CLIENTS]; // to keep client stats after removal
    int stats_count = 0;

    char buffer[BUFFER_SIZE];

    fd_set readfds;
    struct timeval timeout;

    // Record the start time of the channel (in milliseconds).
    DWORD startTime = GetTickCount();

    // Main loop: process incoming connections and channel slots
    while (1) {

        // --- Check for EOF on standard input ---
        // _kbhit() checks if a key press is waiting.
        if (_kbhit()) {
            int c = fgetc(stdin);
            if (c == EOF) {
                // EOF (Ctrl+Z followed by Enter) detected; break the loop to terminate.
                break;
            }
            // Otherwise, ignore the input.
        }

        // --- Accept new connections (non-blocking) ---
        struct sockaddr_in client_addr; // new client sockaddr_in
        int client_addr_len = sizeof(client_addr);


        /*
            the code below accepts all current connections to the channel's socket
            as long as the as the MAX_CLIENTS number hasn't been exceeded.
            if no pending connection accept returns with INVALID_SOCKET
        */
        SOCKET new_sock = accept(channel_sock, (struct sockaddr*)&client_addr, &client_addr_len);
        while (new_sock != INVALID_SOCKET && client_count < MAX_CLIENTS) {
            // new connection socket is valid and we haven’t exceeded MAX_CLIENTS
            clients[client_count].sock = new_sock;
            clients[client_count].addr = client_addr;
            clients[client_count].frames_received = 0;
            clients[client_count].collisions = 0;
            printf("New client connected: %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            client_count++;
            new_sock = accept(channel_sock, (struct sockaddr*)&client_addr, &client_addr_len);
        }

        // If there are no client sockets to monitor, pause for the slot time and continue.
        /*
            avoids select() error 10022 when calling select on empty readfds
            when no client is connecting
        */
        if (client_count == 0) {
            Sleep(slot_time);  // or use an alternative delay mechanism
            continue;
        }

        // --- Set up file descriptor set for the client sockets ---
        /*
            since select() remvoes inactive sockets and there may be new sockets or
            newly active sockets we need to zero and re-set readfds on every loop.
        */
        FD_ZERO(&readfds);
        for (int i = 0; i < client_count; i++) {
            FD_SET(clients[i].sock, &readfds);
        }

        // Set the timeout to the slot_time (convert ms to sec and usec)
        timeout.tv_sec = slot_time / 1000;
        timeout.tv_usec = (slot_time % 1000) * 1000;

        // Wait for incoming data on any client socket
        int activity = select(0, &readfds, NULL, NULL, &timeout);
        if (activity == SOCKET_ERROR) {
            int err = WSAGetLastError();
            fprintf(stderr, "select() error: %d\n", err);
            break;
        }


        // If no client sent data within the slot time, simply continue the loop.
        if (activity == 0) {
            continue;
        }

        // Determine which clients have data ready and store their indexes.
        int ready_count = 0;
        int ready_indexes[MAX_CLIENTS] = { 0 };
        for (int i = 0; i < client_count; i++) {
            // FD_ISSET macro checks if socket recieved data
            if (FD_ISSET(clients[i].sock, &readfds)) {
                ready_indexes[ready_count++] = i;
            }
        }

        // --- Process the slot based on the number of clients ready ---
        if (ready_count == 1) {
            // no collision
            int idx = ready_indexes[0];
            int bytes_received = recv(clients[idx].sock, buffer, BUFFER_SIZE, 0);
            if (bytes_received > 0) {
                clients[idx].frames_received++;
                clients[idx].total_bytes += bytes_received;

                // Broadcast the successfully received frame to all connected clients.
                for (int i = 0; i < client_count; i++) {
                    int bytesSent = send(clients[i].sock, buffer, bytes_received, 0);
                    if (bytesSent == SOCKET_ERROR) {
                        fprintf(stderr, "send() failed to client %s:%d\n",
                            inet_ntoa(clients[i].addr.sin_addr), ntohs(clients[i].addr.sin_port));

                        /* for robustness remove client if send() failed */
                        client_count = removeClient(clients, client_count, idx, all_stats, &stats_count);
                    }
                }
                fprintf(stderr, "Successful transmission from %s:%d, %d bytes broadcasted.\n",
                    inet_ntoa(clients[idx].addr.sin_addr), ntohs(clients[idx].addr.sin_port), bytes_received);
            }
            else if (bytes_received == 0) {
                // The client disconnected gracefully.
                fprintf(stderr, "Client %s:%d disconnected.\n",
                    inet_ntoa(clients[idx].addr.sin_addr), ntohs(clients[idx].addr.sin_port));
                client_count = removeClient(clients, client_count, idx, all_stats, &stats_count);
            }
            else {
                // recv() error
                fprintf(stderr, "recv() error from client %s:%d.\n",
                    inet_ntoa(clients[idx].addr.sin_addr), ntohs(clients[idx].addr.sin_port));
            }
        }
        else if (ready_count > 1) {
            // collision
            for (int j = 0; j < ready_count; j++) {
                int idx = ready_indexes[j];
                int bytes_received = recv(clients[idx].sock, buffer, BUFFER_SIZE, 0);
                if (bytes_received > 0) {
                    clients[idx].collisions++;
                    fprintf(stderr, "Collision detected from %s:%d, received %d bytes (frame discarded).\n",
                        inet_ntoa(clients[idx].addr.sin_addr), ntohs(clients[idx].addr.sin_port), bytes_received);
                }
                else if (bytes_received == 0) {
                    fprintf(stderr, "Client %s:%d disconnected during collision.\n",
                        inet_ntoa(clients[idx].addr.sin_addr), ntohs(clients[idx].addr.sin_port));
                    client_count = removeClient(clients, client_count, idx, all_stats, &stats_count);
                }
                else {
                    fprintf(stderr, "recv() error during collision from client %s:%d.\n",
                        inet_ntoa(clients[idx].addr.sin_addr), ntohs(clients[idx].addr.sin_port));
                }
            }
            // After processing all colliding frames, send the special collision signal to all clients.
            for (int i = 0; i < client_count; i++) {
                int bytesSent = send(clients[i].sock, COLLISION_SIGNAL, COLLISION_SIGNAL_LEN, 0);
                if (bytesSent == SOCKET_ERROR) {
                    fprintf(stderr, "send() collision signal failed to client %s:%d\n",
                        inet_ntoa(clients[i].addr.sin_addr), ntohs(clients[i].addr.sin_port));
                }
            }
            fprintf(stderr, "Collision occurred among %d clients. Collision signal sent to all.\n", ready_count);
        }
        else {
            // If clients were connected to the channel, but no clients were ready - do nothing.
        }
    }

    // Compute the total elapsed time (in seconds) for the channel.
    DWORD elapsed_time_ms = GetTickCount() - startTime;
    double elapsed_time_sec = elapsed_time_ms / 1000.0;

    // Before exiting, display statistics for each client on stderr.
    fprintf(stderr, "\nChannel Statistics:\n");

    for (int i = 0; i < stats_count; i++) {
        double avg_bw = (elapsed_time_sec > 0) ? (all_stats[i].total_bytes / elapsed_time_sec) : 0;
        fprintf(stderr, "Client %s:%d - Frames: %d, Collisions: %d, Average Bandwidth: %.2f bps\n",
            inet_ntoa(all_stats[i].addr.sin_addr), ntohs(all_stats[i].addr.sin_port),
            all_stats[i].frames_received, all_stats[i].collisions, avg_bw);
    }

    // Optionally print still-connected clients
    for (int i = 0; i < client_count; i++) {
        double avg_bw = (elapsed_time_sec > 0) ? (clients[i].total_bytes / elapsed_time_sec) : 0;
        fprintf(stderr, "Client %s:%d - Frames: %d, Collisions: %d, Average Bandwidth: %.2f bps\n",
            inet_ntoa(clients[i].addr.sin_addr), ntohs(clients[i].addr.sin_port),
            clients[i].frames_received, clients[i].collisions, avg_bw);
    }


    // ----- Clean up resources -----
    for (int i = 0; i < client_count; i++) {
        closesocket(clients[i].sock);
    }
    closesocket(channel_sock);
    WSACleanup();
    return 0;
}
