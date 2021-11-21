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
        char buf[512], username[9];
        fgets(buf, 512, stdin);
        sscanf(buf, "%8s", username);
        write(client.fd, username, strlen(username));
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

int get(int fd, size_t size) {
    long long progress = 0;
    char checksum = 0, last_byte = 0;
    while(progress < size) {
        char buf[1024];
        int res = read(client.fd, buf, sizeof(buf));
        if(res < 0) {
            perror("read from remote");
            return -1;
        }
        for(int i = 0; i < res; ++i) checksum ^= buf[i];
        write(fd, buf, res);
        progress += res;
        cerr << progress << '\n';
        last_byte = buf[res-1];
        cerr << "last byte: " << (int) last_byte << '\n';
    }
    checksum ^= last_byte;
    return checksum == last_byte;
}

void get_wrapper(char *filename) {
    FILE *remote = fdopen(dup(client.fd), "r+");
    setvbuf(remote, NULL, _IONBF, 0);
    fprintf(remote, "%d %s\n", GET, filename);
    off_t size;
    fscanf(remote, "%lld", &size);
    fclose(remote);
    cerr << "file size: " << size << '\n';
    if(size < 0) {
        cout << "The " << filename << " doesn't exist\n";
        return;
    }
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    while(true) {
        int res = get(fd, size);
        if(res < 0) return;
        if(res) break;
        ftruncate(fd, 0);
        cerr << "get failed\n";
    }
    ftruncate(fd, size);
    close(fd);
    cout << "get " << filename << " successfully\n";
}

int put(int fd) {
    char buf[1024];
    read(client.fd, buf, 512);
    lseek(fd, 0, SEEK_SET);
    ssize_t count;
    char checksum = 0;
    while((count = read(fd, buf, 1024)) > 0) {
        int res = write(client.fd, buf, count);
        if(res < 0) {
            perror("write to remote");
            return -1;
        }
        for(int i = 0; i < res; ++i) checksum ^= buf[i];
    }
    char remote_checksum;
    read(client.fd, &remote_checksum, 1);
    return checksum == remote_checksum;
}

void put_wrapper(char *filename) {
    int fd = open(filename, O_RDONLY);
    if(fd < 0) {
        cout << "The " << filename << " doesn't exist\n";
        return;
    }
    FILE *remote = fdopen(dup(client.fd), "w");
    setvbuf(remote, NULL, _IONBF, 0);
    fprintf(remote, "%d %s\n%lld\n", PUT, filename, lseek(fd, 0, SEEK_END));
    fclose(remote);
    while(true) {
        int res = put(fd);
        if(res < 0) return;
        if(res) break;
        cerr << "put failed\n";
    }
    cout << "put " << filename << " successfully\n";
}

void bad_format() {
    cout << "Command format error\n";
}

void handle_command() {
    while(true) {
        char command[100], filename[100], line[100];
        fgets(line, 100, stdin);
        int count = sscanf(line, "%s %[^\n]", command, filename);
        if(strcmp(command, "ls") == 0) {
            if(count != 1) {
                bad_format();
                continue;
            }
            ls();
        }
        else if(strcmp(command, "get") == 0) {
            if(count != 2) {
                bad_format();
                continue;
            }
            get_wrapper(filename);
        }
        else if(strcmp(command, "put") == 0) {
            if(count != 2) {
                bad_format();
                continue;
            }
            put_wrapper(filename);
        }
        else {
            cout << "Command not found\n";
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