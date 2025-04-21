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
    if (argc != MAX_ARG_COUNT) {
        printf("Usage: %s <chan_ip> <chan_port> <file_name> <frame_size> <slot_time> <seed> <timeout>\n", argv[0]);
        return 1;
    }

    // Parse arguments
    char* chan_ip = argv[1];
    int chan_port = atoi(argv[2]);
    char* file_name = argv[3];
    int frame_size = atoi(argv[4]);
    int slot_time = atoi(argv[5]);
    int seed = atoi(argv[6]);
    int timeout_sec = atoi(argv[7]);

    srand(seed);

    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        fprintf(stderr, "WSAStartup failed with error: %d\n", result);
        return 1;
    }

    // Create TCP socket
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    // Setup remote address (channel)
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(chan_port);
    server_addr.sin_addr.s_addr = inet_addr(chan_ip);

    // Connect to channel
    result = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (result == SOCKET_ERROR) {
        fprintf(stderr, "Connection failed with error: %d\n", WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    printf("Connected to channel at %s:%d\n", chan_ip, chan_port);

    // Open the file to send
    FILE* file = fopen(file_name, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open file: %s\n", file_name);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Calculate max payload size (frame - header)
    int header_size = sizeof(int); // e.g., sequence number
    int payload_size = frame_size - header_size;
    if (payload_size <= 0) {
        fprintf(stderr, "Frame size too small.\n");
        fclose(file);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    // Statistics variables
    int total_frames = 0;
    int total_bytes = 0;
    int total_transmissions = 0;
    int max_transmissions = 0;
    DWORD start_time = GetTickCount();

    // Read and send frames with ALOHA logic
    char* frame_buf = (char*)malloc(frame_size);
    if (!frame_buf) {
        fprintf(stderr, "Failed to allocate memory for frame buffer.\n");
        fclose(file);
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    int frame_id = 0;
    size_t bytes_read;
    struct timeval tv;
    fd_set readfds;

    while ((bytes_read = fread(frame_buf + header_size, 1, payload_size, file)) > 0) {
        memcpy(frame_buf, &frame_id, header_size);
        int attempts = 0;
        int ack_received = 0;

        while (attempts < MAX_RETRIES && !ack_received) {
            int sent = send(sock, frame_buf, header_size + bytes_read, 0);
            if (sent == SOCKET_ERROR) {
                fprintf(stderr, "Send failed with error: %d\n", WSAGetLastError());
                break;
            }

            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;

            int sel = select(0, &readfds, NULL, NULL, &tv);
            if (sel > 0 && FD_ISSET(sock, &readfds)) {
                char recv_buf[frame_size];
                int received = recv(sock, recv_buf, frame_size, 0);
                if (received > 0) {
                    int recv_id;
                    memcpy(&recv_id, recv_buf, sizeof(int));
                    if (recv_id == frame_id) {
                        ack_received = 1;
                        break;
                    }
                }
            }

            if (!ack_received) {
                attempts++;
                int backoff = rand() % (1 << (attempts < 10 ? attempts : 10));
                printf("Collision detected. Retrying frame %d after %d ms (attempt %d)\n", frame_id, backoff * slot_time, attempts);
                Sleep(backoff * slot_time);
            }
        }

        if (!ack_received) {
            fprintf(stderr, "Frame %d failed to send after %d attempts.\n", frame_id, MAX_RETRIES);
            break;
        }

        total_frames++;
        total_bytes += bytes_read;
        total_transmissions += (attempts + 1);
        if (attempts + 1 > max_transmissions) max_transmissions = attempts + 1;

        printf("Frame %d sent successfully.\n", frame_id);
        frame_id++;
    }

    DWORD end_time = GetTickCount();
    DWORD total_time_ms = end_time - start_time;
    double avg_transmissions = (total_frames > 0) ? (double)total_transmissions / total_frames : 0.0;
    double bandwidth_mbps = (total_time_ms > 0) ? ((total_bytes * 8.0) / 1000000.0) / (total_time_ms / 1000.0) : 0.0;

    fprintf(stderr, "Sent file %s\n", file_name);
    fprintf(stderr, "Result: %s\n", (feof(file) ? "Success :)" : "Failure :("));
    fprintf(stderr, "File size: %d Bytes (%d frames)\n", total_bytes, total_frames);
    fprintf(stderr, "Total transfer time: %lu milliseconds\n", total_time_ms);
    fprintf(stderr, "Transmissions/frame: average %.2f, maximum %d\n", avg_transmissions, max_transmissions);
    fprintf(stderr, "Average bandwidth: %.3f Mbps\n", bandwidth_mbps);

    // Cleanup
    free(frame_buf);
    fclose(file);
    closesocket(sock);
    WSACleanup();
    return 0;
}