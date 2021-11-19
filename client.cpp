#include <bits/stdc++.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "command.h"
using namespace std;

struct {
    int fd;
    sockaddr_in addr;
    socklen_t addrlen;
} client;

void bad_usage(char *progname) {
    cerr << "Usage: " << progname << " [ip:port]\n";
    exit(1);
}

int client_init(int argc, char **argv) {
    if(argc != 2) bad_usage(*argv);
    char *pos = strchr(argv[1], ':');
    if(!pos) bad_usage(*argv);
    *pos = '\0';
    inet_pton(AF_INET, argv[1], &client.addr.sin_addr);
    client.addr.sin_family = AF_INET;
    client.addr.sin_port = htons(atoi(pos+1));
    client.addrlen = sizeof(client.addr);
    client.fd = socket(AF_INET, SOCK_STREAM, 0);
    return connect(client.fd, (sockaddr *) &client.addr, client.addrlen);
}

void close_connection(int status) {
    close(client.fd);
    exit(status);
}

void handle_login() {
    cout << "input your username:\n";
    while(true) {
        string username;
        cin >> username;
        write(client.fd, username.c_str(), username.size());
        char buf[512];
        ssize_t sz = read(client.fd, buf, 512);
        if(sz < 0) {
            perror("login reply");
            close_connection(1);
        }
        switch(*buf) {
            case 'S':
            cout << "connect successfully\n";
            return;
            case 'B':
            cerr << "bad username.";
            close_connection(1);
            case 'U':
            cout << "username is in used, please try another:\n";
            break;
            default:
            cerr << "unrecognised response " << (int) *buf << '\n';
            close_connection(1);
        }
    }
}

void ls() {
    FILE *remote = fdopen(dup(client.fd), "r+");
    fprintf(remote, "%d\n", LS);
    int len;
    fscanf(remote, "%d\n", &len);
    // cerr << "len: " << len << '\n';
    for(int i = 0; i < len; ++i) {
        char buf[512];
        fgets(buf, sizeof(buf), remote);
        cout << buf;
    }
    fclose(remote);
}

void get(string &filename) {
    FILE *remote = fdopen(dup(client.fd), "r+");
    setvbuf(remote, NULL, _IONBF, 0);
    fprintf(remote, "%d %s\n", GET, filename.c_str());
    int size;
    fscanf(remote, "%d", &size);
    fclose(remote);
    cerr << "file size: " << size << '\n';
    if(size < 0) {
        cout << "The " << filename << " doesn't exist\n";
        return;
    }
    int progress = 0;
    int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    while(progress < size) {
        char buf[1024];
        int res = read(client.fd, buf, sizeof(buf));
        if(res < 0) {
            perror("read from remote");
            return;
        }
        write(fd, buf, res);
        progress += res;
        cerr << progress << '\n';
    }
    close(fd);
    cout << "get " << filename << " successfully\n";
}

void handle_command() {
    while(true) {
        string command;
        cin >> command;
        if(command == "ls") {
            ls();
        }
        else if(command == "get") {
            string filename;
            cin >> filename;
            get(filename);
        }
        else if(command == "put") {

        }
    }
}

int main(int argc, char **argv) {
    mkdir("client_dir", 0755);
    chdir("client_dir");
    int res = client_init(argc, argv);
    if(res < 0) {
        perror("connect");
        exit(1);
    }
    handle_login();
    handle_command();
    close_connection(0);
}