#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "s.h"

#define KILO 1024
#define MEGA (1024 * KILO)
#define GIGA (1024 * MEGA)

#define REQUEST_BUFFER_CAPACITY (640 * MEGA)
char request_buffer[REQUEST_BUFFER_CAPACITY];
jmp_buf handle_request_return;

void write_error_response(int fd, int code)
{
    // TODO: not standard
    dprintf(
        fd,
        "HTTP/1.1 %d OMEGALUL\n"
        "Content-Type: text/html\n"
        "\n"
        "<html>"
          "<head>"
            "<title>Error code %d</title>"
          "</head>"
          "<body>"
             "<h1>Error code %d</h1>"
          "</body>"
        "</html>",
        code, code, code);
}

int http_error(int fd, int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    write_error_response(fd, code);
    longjmp(handle_request_return, 1);
}

void response_status_line(int fd, int code)
{
    dprintf(fd, "HTTP/1.1 %d\n", code);
}

void response_header(int fd, const char *name, const char *value_format, ...)
{
    va_list args;
    va_start(args, value_format);

    dprintf(fd, "%s: ", name);
    vdprintf(fd, value_format, args);
    dprintf(fd, "\n");

    va_end(args);
}

void response_body_start(int fd)
{
    dprintf(fd, "\n");
}

void serve_file(int dest_fd,
                const char *filepath,
                const char *content_type)
{
    int src_fd = -1;

    struct stat file_stat;
    int err = stat(filepath, &file_stat);
    if (err < 0) {
        http_error(dest_fd, 404, strerror(errno));
    }

    src_fd = open(filepath, O_RDONLY);
    if (src_fd < 0) {
        http_error(dest_fd, 404, strerror(errno));
    }

    response_status_line(dest_fd, 200);
    response_header(dest_fd, "Content-Type", content_type);
    response_header(dest_fd, "Content-Length", "%d", file_stat.st_size);
    response_body_start(dest_fd);

    off_t offset = 0;
    while (offset < file_stat.st_size) {
        // TODO: align sendfile chunks according to tcp mem buffer
        //     - http://man7.org/linux/man-pages/man2/sysctl.2.html
        //     - `sysctl -w net.ipv4.tcp_mem='8388608 8388608 8388608'`
        ssize_t n = sendfile(dest_fd, src_fd, &offset, 1024);
        if (n < 0) {
            fprintf(stderr, "[ERROR] Could not finish serving the file\n");
            break;
        }
    }

    if (src_fd >= 0) {
        close(src_fd);
    }

    longjmp(handle_request_return, 1);
}

void handle_request(int fd, struct sockaddr_in *addr)
{
    assert(addr);

    ssize_t request_buffer_size = read(fd, request_buffer, REQUEST_BUFFER_CAPACITY);

    if (request_buffer_size == 0) http_error(fd, 400, "EOF");
    if (request_buffer_size < 0)  http_error(fd, 500, strerror(errno));

    String buffer = {
        .len = (uint64_t)request_buffer_size,
        .data = request_buffer
    };

    String line = trim_end(chop_line(&buffer));

    if (!line.len) {
        http_error(fd, 400, "Empty status line\n");
    }

    String method = chop_word(&line);
    if (!string_equal(method, string_null("GET"))) {
        http_error(fd, 405, "Unknown method\n");
    }

    String path = chop_word(&line);
    printf("[%.*s] %.*s\n",
           (int) method.len, method.data,
           (int) path.len, path.data);

    if (string_equal(path, string_null("/"))) {
        serve_file(fd, "./index.html", "text/html");
    }

    if (string_equal(path, string_null("/favicon.png"))) {
        serve_file(fd, "./favicon.png", "image/png");
    }

    http_error(fd, 404, "Unknown path\n");

    // TODO: what if no request terminating function was ever called
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "nodec <port>\n");
        exit(1);
    }

    uint16_t port = 0;

    {
        char *endptr;
        port = (uint16_t) strtoul(argv[1], &endptr, 10);

        if (endptr == argv[1]) {
            fprintf(stderr, "%s is not a port number\n", argv[1]);
            exit(1);
        }
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "Could not create socket epicly: %s\n", strerror(errno));
        exit(1);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    ssize_t err = bind(server_fd, (struct sockaddr*) &server_addr, sizeof(server_addr));
    if (err != 0) {
        fprintf(stderr, "Could not bind socket epicly: %s\n", strerror(errno));
        exit(1);
    }

    err = listen(server_fd, 69);
    if (err != 0) {
        fprintf(stderr, "Could not listen to socket, it's too quiet: %s\n", strerror(errno));
        exit(1);
    }

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_addrlen = 0;
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addrlen);
        if (client_fd < 0) {
            fprintf(stderr, "Could not accept connection. This is unacceptable! %s\n", strerror(errno));
            exit(1);
        }

        assert(client_addrlen == sizeof(client_addr));

        if (setjmp(handle_request_return) == 0) {
            handle_request(client_fd, &client_addr);
        }

        err = close(client_fd);
        if (err < 0) {
            fprintf(stderr, "Could not close client connection: %s\n", strerror(errno));
        }
    }

    return 0;
}
