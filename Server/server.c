// PA1 - Server Module
// This server program sends a file over a TCP connection using a simple ALOHA-like protocol.
// The protocol involves sending fixed-size frames, waiting for an acknowledgment (echo), and
// retransmitting using exponential backoff in case of collisions (simulated by absence of ACK).
// The program reports statistics including average number of transmissions, bandwidth, and timing.

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <windows.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

#define MAX_ARG_COUNT 8
#define MAX_RETRIES 10

int main(int argc, char* argv[]) {
    // Ensure correct number of arguments
    if (argc != MAX_ARG_COUNT) {
        printf("Usage: %s <chan_ip> <chan_port> <file_name> <frame_size> <slot_time> <seed> <timeout>\n", argv[0]);
        return 1;
    }

    // Parse command-line arguments
    char* chan_ip = argv[1];                     // IP of the channel module
    int chan_port = atoi(argv[2]);               // Port to connect to
    char* file_name = argv[3];                   // File to be transmitted
    int frame_size = atoi(argv[4]);              // Total size of one frame (including header)
    int slot_time = atoi(argv[5]);               // Wait time (ms) between retries
    int seed = atoi(argv[6]);                    // Random seed for backoff
    int timeout_sec = atoi(argv[7]);             // Time to wait for ACK before retry (sec)

    srand(seed); // Initialize the random number generator for backoff

    // Initialize Winsock library
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        fprintf(stderr, "WSAStartup failed with error: %d\n", result);
        return 1;
    }

    // Create a TCP socket
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Fill in the server (channel) address struct
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(chan_port);
    server_addr.sin_addr.s_addr = inet_addr(chan_ip);

    // Connect to the channel
    result = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result == SOCKET_ERROR) {
        fprintf(stderr, "Connection failed with error: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("Connected to channel at %s:%d\n", chan_ip, chan_port);

    // Open the file in binary read mode
    FILE* file = fopen(file_name, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open file: %s\n", file_name);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Calculate data payload size (excluding header size)
    int header_size = sizeof(int); // Only frame ID as header
    int payload_size = frame_size - header_size;
    if (payload_size <= 0) {
        fprintf(stderr, "Frame size too small to accommodate header.\n");
        fclose(file);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Statistics initialization
    int total_frames = 0;
    int total_bytes = 0;
    int total_transmissions = 0;
    int max_transmissions = 0;
    DWORD start_time = GetTickCount(); // Track total transmission duration

    // Allocate a buffer for frames
    char* frame_buf = (char*)malloc(frame_size);
    if (!frame_buf) {
        fprintf(stderr, "Failed to allocate memory for frame buffer.\n");
        fclose(file);
        closesocket(sock);
        WSACleanup();
        return 1;
    }
    // Allocate a buffer for receiving echoes
    char* recv_buf = (char*)malloc(frame_size);
    if (!recv_buf) {
        fprintf(stderr, "Memory allocation failed for recv_buf.\n");
        free(frame_buf);
        fclose(file);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Variables for sending loop
    int frame_id = 0;                // Sequence number for frames
    size_t bytes_read;              // Actual data read from file
    struct timeval tv;             // Timeout value for select()
    fd_set readfds;                // File descriptor set for select()

    // Main loop: read and send each frame
    while ((bytes_read = fread(frame_buf + header_size, 1, payload_size, file)) > 0) {
        // Write frame header (just the frame ID)
        memcpy(frame_buf, &frame_id, header_size);
        int attempts = 0;
        int ack_received = 0;

        // Retry loop with exponential backoff
        while (attempts < MAX_RETRIES && !ack_received) {
            // Send the frame
            int sent = send(sock, frame_buf, header_size + bytes_read, 0);
            if (sent == SOCKET_ERROR) {
                fprintf(stderr, "Send failed with error: %d\n", WSAGetLastError());
                break;
            }

            // Use select() to wait for response (ACK) with timeout
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;

            int sel = select(0, &readfds, NULL, NULL, &tv);
            if (sel > 0 && FD_ISSET(sock, &readfds)) {
                // Receive echo/ACK from channel
                
                int received = recv(sock, recv_buf, frame_size, 0);
                if (received > 0) {
                    int recv_id;
                    memcpy(&recv_id, recv_buf, sizeof(int));
                    if (recv_id == frame_id) {
                        ack_received = 1; // ACK matches current frame
                        break;
                    }
                }
            }

            // No ACK or timeout: apply exponential backoff and retry
            if (!ack_received) {
                attempts++;
                int backoff = rand() % (1 << (attempts < 10 ? attempts : 10));
                printf("Collision detected. Retrying frame %d after %d ms (attempt %d)\n",
                       frame_id, backoff * slot_time, attempts);
                Sleep(backoff * slot_time);
            }
        }

        // If no ACK after max retries, abort transfer
        if (!ack_received) {
            fprintf(stderr, "Frame %d failed to send after %d attempts.\n", frame_id, MAX_RETRIES);
            break;
        }

        // Update statistics for this successful frame
        total_frames++;
        total_bytes += bytes_read;
        total_transmissions += (attempts + 1);
        if (attempts + 1 > max_transmissions) max_transmissions = attempts + 1;

        printf("Frame %d sent successfully.\n", frame_id);
        frame_id++;
    }

    // After transmission, calculate statistics
    DWORD end_time = GetTickCount();
    DWORD total_time_ms = end_time - start_time;
    double avg_transmissions = (total_frames > 0) ? (double)total_transmissions / total_frames : 0.0;
    double bandwidth_mbps = (total_time_ms > 0) ? ((total_bytes * 8.0) / 1000000.0) / (total_time_ms / 1000.0) : 0.0;

    // Print summary to stderr (as required)
    fprintf(stderr, "Sent file %s\n", file_name);
    fprintf(stderr, "Result: %s\n", (feof(file) ? "Success :)" : "Failure :("));
    fprintf(stderr, "File size: %d Bytes (%d frames)\n", total_bytes, total_frames);
    fprintf(stderr, "Total transfer time: %lu milliseconds\n", total_time_ms);
    fprintf(stderr, "Transmissions/frame: average %.2f, maximum %d\n", avg_transmissions, max_transmissions);
    fprintf(stderr, "Average bandwidth: %.3f Mbps\n", bandwidth_mbps);

    // Free resources and close everything
    free(recv_buf);
    free(frame_buf);
    fclose(file);
    closesocket(sock);
    WSACleanup();
    return 0;
}