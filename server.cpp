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
    WRITE_DATA, READ_DATA
};

struct {
    struct pollfd *polls;
    state *states;
    int size;
} poll_queue;

struct work {
    off_t progress;
    int fd;
    size_t size;
    state next_state;
    char checksum;
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

void request_write(int conn_fd, int data_fd, size_t size, state next_state) {
    fd_to_work[conn_fd] = {.progress = 0, .fd = data_fd, .size = size, .next_state = next_state, .checksum = 0};
    add_queue(conn_fd, POLLOUT, WRITE_DATA);
}

void request_read(int conn_fd, int data_fd, size_t size, state next_state) {
    fd_to_work[conn_fd] = {.progress = 0, .fd = data_fd, .size = size, .next_state = next_state, .checksum = 0};
    add_queue(conn_fd, POLLIN, READ_DATA);
}

void handle_logout(int fd) {
    cerr << "logout from " << fd << '\n';
    if(fd_to_name.count(fd)) {
        current_users.erase(fd_to_name[fd]);
        fd_to_name.erase(fd);
    }
    fd_to_work.erase(fd);
    close(fd);
}

void accept_connection() {
    int fd = accept(server.fd, (sockaddr *)&server.addr, &server.addrlen);
    add_queue(fd, POLLIN, READ_USERNAME);
    cerr << "fd " << fd << " accepted\n";
}

bool valid_username(string &s) {
    return s.size() > 0;
}

void strip(string &s) {
    int len = s.size();
    for(int i = 0; i < len; ++i) if(!isalnum(s[i])) {
        s.resize(i);
        return;
    }
}

void handle_login(int fd) {
    cerr << "handle login\n";
    char buf[1024];
    ssize_t size = read(fd, buf, 1024);
    if(size < 0) {
        perror("handle login");
        close(fd);
        return;
    }
    buf[size] = '\0';
    string username = buf;
    strip(username);
    if(!valid_username(username)) {
        write(fd, "BADNAME\n", 8);
        close(fd);
        return;
    }
    if(current_users.count(username)) {
        write(fd, "USED\n", 5);
        add_queue(fd, POLLIN, READ_USERNAME);
        return;
    }
    cerr << "got new user " << username << '\n';
    current_users.insert(username);
    fd_to_name[fd] = username;
    write(fd, "SUCCESS\n", 8);
    add_queue(fd, POLLIN, READ_COMMAND);
}

void handle_bad_request(int fd) {
    cerr << "bad request from " << fd << '\n';
    handle_logout(fd);
}

void dir_init() {
    mkdir("server_dir", 0755);
    chdir("server_dir");
}

void server_cleanup(int var) {
    close(server.fd);
    exit(0);
}

void hangup(int var) {
    cerr << "SIGHUP received\n";
}

void broken_pipe(int var) {
    cerr << "SIGPIPE received\n";
}

void write_data(int fd) {
    work w = fd_to_work[fd];
    fd_to_work.erase(fd);
    char buf[1024];
    ssize_t count = pread(w.fd, buf, 1024, w.progress);
    if(count < 0) {
        perror("read from file");
        handle_logout(fd);
        return;
    }
    count = write(fd, buf, count);
    if(count < 0) {
        perror("write to remote");
        handle_logout(fd);
        return;
    }
    for(int i = 0; i < count; ++i) w.checksum ^= buf[i];
    w.progress += count;
    if(w.progress < w.size) {
        fd_to_work[fd] = w;
        add_queue(fd, POLLOUT, WRITE_DATA);
        cerr << fd << " requested another write\n";
    }
    else {
        write(fd, &w.checksum, 1);
        add_queue(fd, POLLIN, w.next_state);
        cerr << fd << " next state: " << w.next_state << '\n';
    }
}

void read_data(int fd) {
    work w = fd_to_work[fd];
    fd_to_work.erase(fd);
    char buf[1024];
    ssize_t count = read(fd, buf, 1024);
    if(count < 0) {
        perror("read from remote");
        handle_login(fd);
        return;
    }
    for(int i = 0; i < count; ++i) w.checksum ^= buf[i];
    write(w.fd, buf, count);
    w.progress += count;
    if(w.progress < w.size) {
        fd_to_work[fd] = w;
        add_queue(fd, POLLIN, READ_DATA);
        cerr << fd << " requested another read\n";
    }
    else {
        write(fd, &w.checksum, 1);
        close(w.fd);
        add_queue(fd, POLLIN, w.next_state);
        cerr << fd << " next state: " << w.next_state << '\n';
    }
}

void ls(int fd) {
    vector<string> dirs;
    for(auto dir : filesystem::directory_iterator(".")) dirs.push_back(dir.path().c_str()+2);
    sort(dirs.begin(), dirs.end());
    string content = to_string(dirs.size()) + '\n';
    for(auto d : dirs) content += d + '\n';
    write(fd, content.c_str(), content.size());
    add_queue(fd, POLLIN, READ_COMMAND);
}

void get(int fd, char *filename) {
    int data_fd = open(filename, O_RDONLY);
    if(data_fd < 0) {
        perror("open for get");
        write(fd, "-1\n", 3);
        add_queue(fd, POLLIN, READ_COMMAND);
    }
    else {
        off_t size = lseek(data_fd, 0, SEEK_END);
        string header = to_string(size) + '\n';
        write(fd, header.c_str(), header.size());
        if(size) request_write(fd, data_fd, size, READ_COMMAND);
        else add_queue(fd, POLLIN, READ_COMMAND);
    }
}

void put(int fd, char *filename, size_t filesize) {
    int data_fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if(filesize == 0) {
        cerr << "zero file size\n";
        close(data_fd);
        add_queue(fd, POLLIN, READ_COMMAND);
        return;
    }
    request_read(fd, data_fd, filesize, READ_COMMAND);
    write(fd, "go on\n", 6);
}

void read_command(int fd) {
    cerr << "reading command\n";
    char input[512] = {0}, filename[512] = {0};
    read(fd, input, 512);
    command choice;
    size_t filesize;
    int cnt = sscanf(input, "%d%s%lld", &choice, filename, &filesize);
    cerr << "command cnt: " << cnt << '\n';
    switch(choice) {
        case LS:
        cnt == 1 ? ls(fd) : handle_bad_request(fd);
        break;
        case GET:
        cnt == 2 ? get(fd, filename) : handle_bad_request(fd);
        break;
        case PUT:
        cnt == 3 ? put(fd, filename, filesize) : handle_bad_request(fd);
        break;
        default:
        handle_bad_request(fd);
    }
}

int main(int argc, char **argv) {
    server_init(argc, argv);
    queue_init();
    dir_init();
    signal(SIGINT, server_cleanup);
    signal(SIGHUP, hangup);
    signal(SIGPIPE, broken_pipe);

    while(true) {
        cerr << "poll size: " << poll_queue.size << '\n';
        if(poll(poll_queue.polls, poll_queue.size, -1) < 0) perror("polling");
        short revents;
        for(int i = 0; i < poll_queue.size; ++i) if(revents = poll_queue.polls[i].revents) {
            if(poll_queue.states[i] == ACCEPT_CONNECTION) {
                accept_connection();
                continue;
            }
            if(revents & (POLLHUP | POLLERR | POLLNVAL)) handle_logout(poll_queue.polls[i].fd);
            else switch(poll_queue.states[i]) {
                case READ_USERNAME:
                handle_login(poll_queue.polls[i].fd);
                break;
                case READ_COMMAND:
                read_command(poll_queue.polls[i].fd);
                break;
                case WRITE_DATA:
                write_data(poll_queue.polls[i].fd);
                break;
                case READ_DATA:
                read_data(poll_queue.polls[i].fd);
                break;
                default:
                cerr << "unrecognised state " << poll_queue.states[i] << '\n';
                handle_bad_request(poll_queue.polls[i].fd);
                break;
            }
            remove_queue(i--);
        }
    }
}