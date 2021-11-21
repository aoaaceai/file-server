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

void request_write(int conn_fd, work_method method, int data_fd, char *buf, size_t buffsize, state next_state) {
    fd_to_work[conn_fd] = {.method = method, .progress = 0, .fd = data_fd, .buf = buf, .buffsize = buffsize, .next_state = next_state, .checksum = 0};
    add_queue(conn_fd, POLLOUT, WRITE_DATA);
}

void request_read(int conn_fd, int data_fd, state next_state, size_t data_size) {
    fd_to_work[conn_fd] = {.method = FROM_FD, .progress = 0, .fd = data_fd, .buf = NULL, .buffsize = data_size, .next_state = next_state, .checksum = 0};
    add_queue(conn_fd, POLLIN, READ_DATA);
}

void handle_logout(int fd) {
    if(fd_to_name.count(fd)) {
        current_users.erase(fd_to_name[fd]);
        fd_to_name.erase(fd);
    }
    if(fd_to_work.count(fd)) delete fd_to_work[fd].buf;
    fd_to_work.erase(fd);
    close(fd);
}

void handle_write(int conn_fd) {
    cerr << "handle write to " << conn_fd << '\n';
    work w = fd_to_work[conn_fd];
    fd_to_work.erase(conn_fd);
    if(w.method == FROM_BUF) {
        ssize_t res = write(conn_fd, w.buf + w.progress, w.buffsize - w.progress);
        if(res < 0) {
            perror("write to remote");
            handle_logout(conn_fd);
            return;
        }
        w.progress += res;
    }
    else {
        if(w.progress == 0) {
            off_t sz = lseek(w.fd, 0, SEEK_END);
            w.buffsize = sz;
            FILE *remote = fdopen(dup(conn_fd), "r+");
            setvbuf(remote, NULL, _IONBF, 0);
            fprintf(remote, "%lld\n", sz);
            fclose(remote);
        }
        char buf[1024];
        ssize_t res = pread(w.fd, buf, 1024, w.progress);
        if(res < 0) {
            perror("read from file");
            return;
        }
        for(int i = 0; i < res; ++i) w.checksum ^= buf[i];
        // cerr << "bytes read: " << res << '\n';
        res = write(conn_fd, buf, res);
        if(res < 0) {
            perror("write to remote");
            handle_logout(conn_fd);
            return;
        }
        cerr << "bytes wrote: " << res << '\n';
        w.progress += res;
        cerr << "checksum: " << (int) w.checksum << '\n';
    }
    if(w.progress < w.buffsize) {
        fd_to_work[conn_fd] = w;
        add_queue(conn_fd, POLLOUT, WRITE_DATA);
        cerr << conn_fd << " requested another write\n";

    }
    else {
        w.method == FROM_FD && write(conn_fd, &w.checksum, 1);
        delete w.buf;
        add_queue(conn_fd, POLLIN, w.next_state);
        cerr << conn_fd << " next state: " << w.next_state << '\n';
    }
}

int get_filesize(int fd) {
    char buf[20] = {0};
    int len = 0;
    while(true) {
        int res = read(fd, buf+len, 1);
        if(res != 1) return -1;
        cerr << (int) buf[len] << ' ';
        if(!isdigit(buf[len])) {
            break;
        }
        ++len;
    }
    cerr << '\n';
    return atoi(buf);
}

void handle_read(int conn_fd) {
    cerr << "handle read for " << conn_fd << '\n';
    work w = fd_to_work[conn_fd];
    fd_to_work.erase(conn_fd);
    char buf[1024];
    ssize_t count = read(conn_fd, buf, 1024);
    if(count < 0) {
        cerr << "read from remote\n";
        handle_logout(conn_fd);
        return;
    }
    for(int i = 0; i < count; ++i) w.checksum ^= buf[i];
    write(w.fd, buf, count);
    w.progress += count;
    if(w.progress < w.buffsize) {
        fd_to_work[conn_fd] = w;
        add_queue(conn_fd, POLLIN, READ_DATA);
        cerr << conn_fd << " requested another read\n";
    }
    else {
        // output checksum
        write(conn_fd, &w.checksum, 1);
        close(w.fd);
        add_queue(conn_fd, POLLIN, w.next_state);
        cerr << conn_fd << " next state: " << w.next_state << '\n';
    }
}

void accept_connection() {
    int fd = accept(server.fd, (sockaddr *)&server.addr, &server.addrlen);
    add_queue(fd, POLLIN, READ_USERNAME);
    cerr << "fd " << fd << " accepted\n";
}

void strip(string &s) {
    int len = s.size();
    for(int i = 0; i < len; ++i) if(!isalnum(s[i])) {
        s.resize(i);
        return;
    }
}

bool valid_username(string &s) {
    return s.size() > 0;
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

void ls(int fd) {
    cerr << "processing ls\n";
    vector<string> dirs;
    for(auto entry : filesystem::directory_iterator(".")) dirs.push_back(entry.path().c_str()+2);
    sort(dirs.begin(), dirs.end());
    string content = to_string(dirs.size()) + '\n';
    for(auto d : dirs) content += d + '\n';
    size_t buffsize = content.size();
    char *buf = new char[buffsize];
    strcpy(buf, content.c_str());
    request_write(fd, FROM_BUF, -1, buf, buffsize, READ_COMMAND);
}

void dump_file(int fd, char *filename) {
    int data_fd = open(filename, O_RDONLY);
    request_write(fd, FROM_FD, data_fd, NULL, 0, READ_COMMAND);
}

void get(int fd, char *filename) {
    cerr << "checking " << filename << '\n';
    struct stat buf;
    if(stat(filename, &buf) == 0) {
        dump_file(fd, filename);
    }
    else {
        char *header = new char[40];
        int len = sprintf(header, "%lld\n", -1LL);
        cerr << header << '\n';
        request_write(fd, FROM_BUF, -1, header, len, READ_COMMAND);
    }
}

void put(int fd, char *filename, size_t filesize) {
    cerr << "trying to put " << filename << " with size " << filesize << '\n';
    int data_fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    request_read(fd, data_fd, READ_COMMAND, filesize);
    write(fd, "go on", 5);
}

void read_command(int fd) {
    cerr << "reading command\n";
    char input[512], filename[512];
    read(fd, input, 512);
    command choice = BAD_COMMAND;
    sscanf(input, "%d %[^\r\n]\n", &choice, filename);
    int cnt;
    cerr << "got command input " << choice << '\n';
    switch(choice) {
        case LS:
        ls(fd);
        break;
        case GET:
        get(fd, filename);
        break;
        case PUT:
        char buf[512];
        cnt = sprintf(buf, "%d %s\n", choice, filename);
        put(fd, filename, atoll(input+cnt));
        break;
        default:
        handle_bad_request(fd);
    }
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
            // cerr << "got event from " << poll_queue.polls[i].fd << '\n';
            if(revents & (POLLHUP | POLLERR | POLLNVAL)) {
                handle_logout(poll_queue.polls[i].fd);
            }
            else switch(poll_queue.states[i]) {
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
                case READ_DATA:
                handle_read(poll_queue.polls[i].fd);
                break;
                default:
                cerr << "unrecognised state " << poll_queue.states[i] << " from " << poll_queue.polls[i].fd << '\n';
            }
            remove_queue(i--);
        }
    }
}