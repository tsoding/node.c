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
#include "json.h"

#define REQUEST_BUFFER_CAPACITY (640 * KILO)
char request_buffer[REQUEST_BUFFER_CAPACITY];

void http_error_page_template(int OUT, int code)
{
#define INT(x) dprintf(OUT, "%d", x);
#include "error_page_template.h"
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
        // TODO(#3): Try to align sendfile chunks according to tcp mem buffer
        //     Will that even improve the performance?
        //     References:
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

int is_cancelled(struct Schedule *schedule, time_t id)
{
    for (size_t i = 0; i < schedule->cancelled_events_count; ++i) {
        if (schedule->cancelled_events[i] == id) {
            return 1;
        }
    }
    return 0;
}

// TODO(#13): schedule does not support patches
// TODO(#10): there is no endpoint to get a schedule for a period
// TODO(#14): / should probably return the page of https://tsoding.org/schedule
//   Which will require to move rest map to somewhere

time_t id_of_event(struct Event event)
{
    return timegm(&event.date) + timezone + event.time_min * 60;
}

Json_Value event_as_json(Memory *memory, struct Event event)
{
    assert(memory);
    assert(!"TODO(#42): event_as_json is not implemented");
    (void) event;

    // write(dest_fd, "{", 1);
    // print_json_string_literal(dest_fd, "id");
    // write(dest_fd, ":", 1);
    // dprintf(dest_fd, "%ld", id_of_event(event));
    // write(dest_fd, ",", 1);
    // print_json_string_literal(dest_fd, "title");
    // write(dest_fd, ":", 1);
    // print_json_string_literal(dest_fd, event.title);
    // write(dest_fd, ",", 1);
    // print_json_string_literal(dest_fd, "description");
    // write(dest_fd, ":", 1);
    // print_json_string_literal(dest_fd, event.description);
    // write(dest_fd, ",", 1);
    // print_json_string_literal(dest_fd, "url");
    // write(dest_fd, ":", 1);
    // print_json_string_literal(dest_fd, event.url);
    // write(dest_fd, "}", 1);
    // write(dest_fd, "\n", 1);

    return json_null;
}

void print_event(int dest_fd, Memory *memory, struct Event event)
{
    Json_Value event_json = event_as_json(memory, event);
    print_json_value_fd(dest_fd, event_json);
}

int next_event(time_t current_time,
               struct Schedule *schedule,
               struct Event *output)
{
    struct Event result = {0};
    time_t result_id = -1;

    for (size_t i = 0; i < schedule->extra_events_size; ++i) {
        struct Event event = schedule->extra_events[i];
        time_t event_id = id_of_event(event);
        if (current_time < event_id && !is_cancelled(schedule, event_id)) {
            if (result_id < 0 || event_id < result_id) {
                result = event;
                result_id = event_id;
            }
        }
    }

    for (int j = 0; j < 7; ++j) {
        time_t week_time = current_time + 24 * 60 * 60 * j;
        struct tm *week_tm = gmtime(&week_time);

        for (size_t i = 0; i < schedule->projects_size; ++i) {
            if (!(schedule->projects[i].days & (1 << week_tm->tm_wday))) {
                continue;
            }

            if (schedule->projects[i].starts) {
                time_t starts_time = timegm(schedule->projects[i].starts) - timezone;
                if (week_time < starts_time) continue;
            }

            if (schedule->projects[i].ends) {
                time_t ends_time = timegm(schedule->projects[i].ends) - timezone;
                if (ends_time < week_time) continue;
            }

            struct Event event = {
                .time_min = schedule->projects[i].time_min,
                .title = schedule->projects[i].name,
                .description = schedule->projects[i].description,
                .url = schedule->projects[i].url,
                .channel = schedule->projects[i].channel
            };

            event.date = *week_tm;
            event.date.tm_sec = 0;
            event.date.tm_min = 0;
            event.date.tm_hour = 0;

            time_t event_id = id_of_event(event);

            if (is_cancelled(schedule, event_id)) {
                continue;
            }

            if (current_time >= event_id) {
                continue;
            }

            if (result_id < 0 || event_id < result_id) {
                result = event;
                result_id = event_id;
            }
        }
    }

    if (output) {
        *output = result;
    }

    return result_id >= 0;
}

int serve_next_stream(int dest_fd, Memory *memory, struct Schedule *schedule)
{
    response_status_line(dest_fd, 200);
    response_header(dest_fd, "Content-Type", "application/json");
    response_body_start(dest_fd);

    time_t current_time = time(NULL) - timezone;
    struct Event event;
    if (next_event(current_time, schedule, &event)) {
        print_event(dest_fd, memory, event);
    }

    return 0;
}

int serve_rest_map(int dest_fd, String host)
{
    (void) dest_fd;
    (void) host;
    assert(!"TODO(#43): serve_rest_map is not implemented");
    // {
    //     "next_stream":  "%PROTOCOL%://%HOST%/next_stream"
    // }
    return 0;
}

int handle_request(int fd, struct sockaddr_in *addr, Memory *memory, struct Schedule *schedule)
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

    String host = {0};
    String header_line = trim(chop_line(&buffer));
    Header header = {0};
    while (header_line.len > 0) {
        header = parse_header(header_line);
        if (string_equal(header.name, SLT("Host"))) {
            host = header.value;
        }

        header_line = trim(chop_line(&buffer));
    }

    if (string_equal(status_line.path, SLT("/"))) {
        return serve_rest_map(fd, host);
    }

    if (string_equal(status_line.path, SLT("/favicon.png"))) {
        return serve_file(fd, "./favicon.png", "image/png");
    }

    if (string_equal(status_line.path, SLT("/next_stream"))) {
        return serve_next_stream(fd, memory, schedule);
    }

    return http_error(fd, 404, "Unknown path\n");
}

#define MEMORY_CAPACITY (1 * MEGA)

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
    if (argc < 3) {
        fprintf(stderr, "skedudle <schedule.json> <port> [address]\n");
        exit(1);
    }

    const char *filepath = argv[1];
    const char *port_cstr = argv[2];
    const char *addr = "127.0.0.1";
    if (argc >= 4) {
        addr = argv[3];
    }

    Memory json_memory = {
        .capacity = MEMORY_CAPACITY,
        .buffer = malloc(MEMORY_CAPACITY)
    };
    assert(json_memory.buffer);

    Memory request_memory = {
        .capacity = MEMORY_CAPACITY,
        .buffer = malloc(MEMORY_CAPACITY)
    };
    assert(request_memory.buffer);

    String input = mmap_file_to_string(filepath);
    Json_Result result = parse_json_value(&json_memory, input);
    if (result.is_error) {
        print_json_error(stderr, result, input, filepath);
        exit(1);
    }
    printf("Parsing consumed %ld bytes of memory\n", json_memory.size);
    struct Schedule schedule = json_as_schedule(&json_memory, result.value);
    munmap_string(input);

    if (schedule.timezone == NULL) {
        fprintf(stderr, "Timezone is not provided in the json file\n");
        exit(1);
    }

    printf("Schedule timezone: %s\n", schedule.timezone);

    char schedule_timezone[256];
    snprintf(schedule_timezone, 256, ":%s", schedule.timezone);
    setenv("TZ", schedule_timezone, 1);
    tzset();

    uint16_t port = 0;

    {
        char *endptr;
        port = (uint16_t) strtoul(port_cstr, &endptr, 10);

        if (endptr == port_cstr) {
            fprintf(stderr, "%s is not a port number\n", port_cstr);
            exit(1);
        }
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

        handle_request(client_fd, &client_addr, &request_memory, &schedule);

        err = close(client_fd);
        if (err < 0) {
            fprintf(stderr, "Could not close client connection: %s\n", strerror(errno));
        }
    }

    free(json_memory.buffer);
    free(request_memory.buffer);

    return 0;
}
