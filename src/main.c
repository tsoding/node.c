#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <time.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "s.h"
#include "response.h"
#include "request.h"
#include "memory.h"
#include "schedule.h"
#include "frozen.h"

#define REQUEST_BUFFER_CAPACITY (640 * KILO)
char request_buffer[REQUEST_BUFFER_CAPACITY];

void http_error_page_template(int OUT, int code)
{
#define INT(x) dprintf(OUT, "%d", x);
#include "error_page.h"
#undef INT
}

int http_error(int fd, int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    response_status_line(fd, code);
    response_header(fd, "Content-Type", "text/html");
    response_body_start(fd);
    http_error_page_template(fd, code);

    return 1;
}

int serve_file(int dest_fd,
                const char *filepath,
                const char *content_type)
{
    int src_fd = -1;

    struct stat file_stat;
    int err = stat(filepath, &file_stat);
    if (err < 0) {
        return http_error(dest_fd, 404, strerror(errno));
    }

    src_fd = open(filepath, O_RDONLY);
    if (src_fd < 0) {
        return http_error(dest_fd, 404, strerror(errno));
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

    return 0;
}

int print_json_escaped_string(int fd, const char *p, size_t len) {
    size_t i, cl, n = 0;
    const char *hex_digits = "0123456789abcdef";
    const char *specials = "btnvfr";

    for (i = 0; i < len; i++) {
        unsigned char ch = ((unsigned char *) p)[i];
        if (ch == '"' || ch == '\\') {
            n += write(fd, "\\", 1);
            n += write(fd, p + i, 1);
        } else if (ch >= '\b' && ch <= '\r') {
            n += write(fd, "\\", 1);
            n += write(fd, &specials[ch - '\b'], 1);
        } else if (isprint(ch)) {
            n += write(fd, p + i, 1);
        } else if ((cl = json_get_utf8_char_len(ch)) == 1) {
            n += write(fd, "\\u00", 4);
            n += write(fd, &hex_digits[(ch >> 4) % 0xf], 1);
            n += write(fd, &hex_digits[ch % 0xf], 1);
        } else {
            n += write(fd, p + i, cl);
            i += cl - 1;
        }
    }

    return n;
}

void print_json_string_literal(int fd, const char *literal)
{
    write(fd, "\"", 1);
    print_json_escaped_string(fd, literal, strlen(literal));
    write(fd, "\"", 1);
}

int serve_projects_list(int dest_fd, struct Schedule *schedule)
{
    response_status_line(dest_fd, 200);
    response_header(dest_fd, "Content-Type", "application/json");
    response_body_start(dest_fd);

    write(dest_fd, "[", 1);
    for (size_t i = 0; i < schedule->projects_size; ++i) {
        if (i > 0) write(dest_fd, ",", 1);
        print_json_string_literal(dest_fd, schedule->projects[i].name);
    }
    write(dest_fd, "]\n", 2);

    return 0;
}

int serve_next_stream(int dest_fd, struct Schedule *schedule)
{
    response_status_line(dest_fd, 200);
    response_header(dest_fd, "Content-Type", "application/json");
    response_body_start(dest_fd);

    time_t current_time = time(NULL) - timezone;
    struct tm *current_tm = gmtime(&current_time);

    // TODO: serve_next_stream is unfinished
    for (size_t i = 0; i < schedule->projects_size; ++i) {
        int x = (current_tm->tm_wday + 6) % 7;
        if (schedule->projects[i].days & (1 << x)) {
            print_json_string_literal(dest_fd, schedule->projects[i].name);
            write(dest_fd, "\n", 1);
        }
    }

    return 0;
}

int handle_request(int fd, struct sockaddr_in *addr, struct Schedule *schedule)
{
    assert(addr);

    ssize_t request_buffer_size = read(fd, request_buffer, REQUEST_BUFFER_CAPACITY);

    if (request_buffer_size == 0) return http_error(fd, 400, "EOF");
    if (request_buffer_size < 0)  return http_error(fd, 500, strerror(errno));

    String buffer = {
        .len = (uint64_t)request_buffer_size,
        .data = request_buffer
    };

    Status_Line status_line = chop_status_line(&buffer);

    if (!string_equal(status_line.method, SLT("GET"))) {
        return http_error(fd, 405, "Unknown method\n");
    }
    printf("[%.*s] %.*s\n",
           (int) status_line.method.len, status_line.method.data,
           (int) status_line.path.len, status_line.path.data);

    if (string_equal(status_line.path, SLT("/"))) {
        return serve_file(fd, "./index.html", "text/html");
    }

    if (string_equal(status_line.path, SLT("/favicon.png"))) {
        return serve_file(fd, "./favicon.png", "image/png");
    }

    if (string_equal(status_line.path, SLT("/projects"))) {
        return serve_projects_list(fd, schedule);
    }

    if (string_equal(status_line.path, SLT("/next_stream"))) {
        return serve_next_stream(fd, schedule);
    }

    return http_error(fd, 404, "Unknown path\n");
}

#define MEMORY_CAPACITY (640 * KILO)

String mmap_file_to_string(const char *filepath)
{
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open file `%s'\n", filepath);
        abort();
    }

    struct stat fd_stat;
    int err = fstat(fd, &fd_stat);
    assert(err == 0);

    String result;
    result.len = fd_stat.st_size;
    result.data = mmap(NULL, result.len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    assert(result.data);
    close(fd);

    return result;
}

void munmap_string(String s)
{
    munmap((void*) s.data, s.len);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "skedudle <port> [address]\n");
        exit(1);
    }

    // TODO: schedule.json file is hardcoded

    const char *filepath = "./schedule.json";

    Memory json_memory = {0};
    json_memory.capacity = MEMORY_CAPACITY;
    json_memory.buffer = malloc(MEMORY_CAPACITY);

    String input = mmap_file_to_string(filepath);
    struct Schedule schedule;
    json_scan_schedule(&json_memory, input, &schedule);
    munmap_string(input);

    printf("Schedule timezone: %s\n", schedule.timezone);

    char schedule_timezone[256];
    snprintf(schedule_timezone, 256, ":%s", schedule.timezone);
    setenv("TZ", schedule_timezone, 1);
    tzset();

    uint16_t port = 0;

    {
        char *endptr;
        port = (uint16_t) strtoul(argv[1], &endptr, 10);

        if (endptr == argv[1]) {
            fprintf(stderr, "%s is not a port number\n", argv[1]);
            exit(1);
        }
    }

    const char *addr = "127.0.0.1";
    if (argc >= 3) {
        addr = argv[2];
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "Could not create socket epicly: %s\n", strerror(errno));
        exit(1);
    }
    int option = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(addr);

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

    printf("[INFO] Listening to http://%s:%d/\n", addr, port);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_addrlen = 0;
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addrlen);
        if (client_fd < 0) {
            fprintf(stderr, "Could not accept connection. This is unacceptable! %s\n", strerror(errno));
            exit(1);
        }

        assert(client_addrlen == sizeof(client_addr));

        handle_request(client_fd, &client_addr, &schedule);

        err = close(client_fd);
        if (err < 0) {
            fprintf(stderr, "Could not close client connection: %s\n", strerror(errno));
        }
    }

    free(json_memory.buffer);

    return 0;
}
