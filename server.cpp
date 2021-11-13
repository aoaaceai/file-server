#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <bits/stdc++.h>
#include <poll.h>
#include "command.h"
#include <filesystem>
using namespace std;

struct {
    int fd;
    struct sockaddr_in addr;
    socklen_t addrlen;
    int maxconn;
} server;

enum state {
    ACCEPT_CONNECTION, READ_USERNAME, READ_COMMAND,
    WRITE_DATA
};

struct {
    struct pollfd *polls;
    state *states;
    int size;
} poll_queue;

enum work_method {
    FROM_FD, FROM_BUF
};

struct work {
    work_method method;
    off_t progress;
    int fd;
    char *buf;
    size_t buffsize;
    state next_state;
};

unordered_set<string> current_users;
unordered_map<int, string> fd_to_name;
unordered_map<int, work> fd_to_work;

void bad_usage(char *progname) {
    cerr << "Usage: " << progname << " [port]\n";
    exit(1);
}

void server_init(int argc, char **argv) {
    if(argc != 2) bad_usage(*argv);
    int port = atoi(argv[1]);
    if(port <= 0 || port >= 65536) bad_usage(*argv);

    server.addr.sin_port = htons(port);
    server.addr.sin_family = AF_INET;
    server.addr.sin_addr.s_addr = INADDR_ANY;
    server.addrlen = sizeof(server.addr);
    server.maxconn = getdtablesize();
    server.fd = socket(AF_INET, SOCK_STREAM, 0);

    int res = bind(server.fd, (sockaddr *)&server.addr, server.addrlen);
    if(res < 0) {
        perror("bind");
        exit(1);
    }
    listen(server.fd, 20);
}

void add_queue(int fd, short events, state state) {
    poll_queue.polls[poll_queue.size] = {fd, events, 0};
    poll_queue.states[poll_queue.size++] = state;
}

void remove_queue(int index) {
    poll_queue.polls[index] = poll_queue.polls[--poll_queue.size];
    poll_queue.states[index] = poll_queue.states[poll_queue.size];
}

void queue_init() {
    poll_queue.size = 0;
    poll_queue.polls = new struct pollfd[server.maxconn];
    poll_queue.states = new state[server.maxconn];
    add_queue(server.fd, POLLIN, ACCEPT_CONNECTION);
}

void request_write(int conn_fd, work_method method, int data_fd, char *buf, size_t buffsize, state next_state) {
    fd_to_work[conn_fd] = {.method = method, .progress = 0, .fd = data_fd, .buf = buf, .buffsize = buffsize, .next_state = next_state};
    add_queue(conn_fd, POLLOUT, WRITE_DATA);
}

void handle_write(int conn_fd) {
    cerr << "handle write\n";
    fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL) | O_NONBLOCK);
    work w = fd_to_work[conn_fd];
    fd_to_work.erase(conn_fd);
    if(w.method == FROM_BUF) {
        ssize_t res = write(w.fd, w.buf + w.progress, w.buffsize - w.progress);
        if(res < 0) {
            perror("write to remote");
            fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL) | ~O_NONBLOCK);
            return;
        }
        w.progress += res;
    }
    else if(w.method == FROM_FD) {
        char buf[1024];
        ssize_t res = pread(w.fd, buf, 1024, w.progress);
        if(res < 0) {
            perror("read from file");
            fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL) & ~O_NONBLOCK);
            return;
        }
        res = write(w.fd, buf, res);
        if(res < 0) {
            perror("write to remote");
            fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL) & ~O_NONBLOCK);
            return;
        }
        w.progress += res;
    }
    fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL) & ~O_NONBLOCK);
    if(w.progress < w.buffsize) fd_to_work[conn_fd] = w;
    else delete w.buf;
}

void accept_connection() {
    int fd = accept(server.fd, (sockaddr *)&server.addr, &server.addrlen);
    add_queue(fd, POLLIN, READ_USERNAME);
}

bool valid_username(string &s) {
    if(s.length() <= 2) return false;
    for(char c : s) {
        if(c == '\r') return true;
        else if(!isalnum(c)) {
            // cerr << (int) c << '\n';
            return false;
        }
    }
    return true;
}

void handle_login(int fd) {
    char buf[1024];
    ssize_t size = read(fd, buf, 1024);
    if(size < 0) {
        perror("handle login");
        close(fd);
        return;
    }
    buf[size] = '\0';
    string username = buf;
    if(!valid_username(username)) {
        write(fd, "login BADNAME\n", 14);
        close(fd);
        return;
    }
    if(current_users.count(username)) {
        write(fd, "login USED\n", 11);
        close(fd);
        return;
    }
    current_users.insert(username);
    fd_to_name[fd] = username;
    write(fd, "login SUCCESS\n", 14);
    add_queue(fd, POLLIN, READ_COMMAND);
}

void handle_logout() {
    // TODO
}

void handle_bad_request(int fd) {
    cerr << "bad request from " << fd << '\n';
    // TODO
}

void ls(int fd) {
    vector<string> dirs;
    for(auto entry : filesystem::directory_iterator("server_dir")) dirs.push_back((string)entry.path());
    sort(dirs.begin(), dirs.end());
    string content = to_string(dirs.size()) + '\n';
    for(auto d : dirs) content += d + '\n';
    size_t buffsize = content.size();
    char *buf = new char[buffsize];
    strcpy(buf, content.c_str());
    request_write(fd, FROM_BUF, 0, buf, buffsize, READ_COMMAND);
}

void read_command(int fd) {
    char input[512], filename[512];
    read(fd, input, 512);
    command choice;
    sscanf(input, "%d%s", &choice, filename);
    switch(choice) {
        case LS:
        ls(fd);
        break;
        case GET:
        break;
        case PUT:
        break;
        default:
        handle_bad_request(fd);
    }
}

void dir_init() {
    mkdir("server_dir", 0755);
}

int main(int argc, char **argv) {
    server_init(argc, argv);
    queue_init();
    dir_init();
    while(true) {
        if(poll(poll_queue.polls, poll_queue.size, -1) < 0) perror("polling");
        short revents;
        for(int i = 0; i < poll_queue.size; ++i) if(revents = poll_queue.polls[i].revents) {
            // handle bad revents?
            // POLLHUP, etc
            if(poll_queue.states[i] == ACCEPT_CONNECTION) {
                accept_connection();
                continue;
            }
            switch(poll_queue.states[i]) {
                case ACCEPT_CONNECTION:
                accept_connection();
                break;
                case READ_USERNAME:
                handle_login(poll_queue.polls[i].fd);
                break;
                case READ_COMMAND:
                read_command(poll_queue.polls[i].fd);
                break;
                case WRITE_DATA:
                handle_write(poll_queue.polls[i].fd);
                break;
            }
            remove_queue(i--);
        }
    }
}