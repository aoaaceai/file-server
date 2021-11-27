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

void dir_init() {
    mkdir("client_dir", 0755);
    chdir("client_dir");
}

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
    printf("input your username:\n");
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
            printf("connect successfully\n");
            return;
            case 'B':
            cerr << "bad username";
            case 'U':
            printf("username is in used, please try another:\n");
            break;
            default:
            cerr << "unrecognised response " << buf << '\n';
            close_connection(1);
        }
    }
}

void ls() {
    FILE *remote = fdopen(dup(client.fd), "r+");
    fprintf(remote, "%d\n", LS);
    int len;
    char buf[512];
    fgets(buf, 512, remote);
    sscanf(buf, "%d\n", &len);
    for(int i = 0; i < len; ++i) {
        fgets(buf, sizeof(buf), remote);
        printf("%s", buf);
    }
    fclose(remote);
}

int get(int fd, size_t size) {
    size_t progress = 0;
    char checksum = 0, last_byte = 0;
    while(progress <= size) {
        char buf[1024];
        int res = read(client.fd, buf, sizeof(buf));
        if(res < 0) {
            perror("read from remote");
            return -1;
        }
        for(int i = 0; i < res; ++i) checksum ^= buf[i];
        write(fd, buf, res);
        progress += res;
        last_byte = buf[res-1];
    }
    checksum ^= last_byte;
    return checksum == last_byte;
}

void get_wrapper(char *filename) {
    FILE *remote = fdopen(dup(client.fd), "r+");
    setvbuf(remote, NULL, _IONBF, 0);
    fprintf(remote, "%d %s\n", GET, filename);
    off_t size = -1;
    fscanf(remote, "%lld", &size);
    if(size < 0) {
        printf("The %s doesn't exist\n", filename);
        return;
    }
    else if(size == 0) {
        cerr << "zero file size\n";
        int fd = open(filename, O_CREAT | O_TRUNC, 0600);
        close(fd);
        printf("get %s successfully\n", filename);
        return;
    }
    cerr << "file size: " << size << '\n';
    int fd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    while(true) {
        int res = get(fd, size);
        if(res < 0) {
            close(fd);
            return;
        }
        if(res) break;
        ftruncate(fd, 0);
        cerr << "get failed\n";
    }
    ftruncate(fd, size);
    close(fd);
    printf("get %s successfully\n", filename);
}

int put(int fd) {
    char buf[1024];
    // "go on" from remote
    read(client.fd, buf, 512);
    lseek(fd, 0, SEEK_SET);
    ssize_t count;
    char checksum = 0;
    while((count = read(fd, buf, 1024)) > 0) {
        cerr << "read\n";
        int res = write(client.fd, buf, count);
        cerr << "write\n";
        if(res < 0) {
            perror("write to remote");
            return -1;
        }
        for(int i = 0; i < res; ++i) checksum ^= buf[i];
        cerr << res << ' ' << (int) checksum << '\n';
    }
    char remote_checksum;
    read(client.fd, &remote_checksum, 1);
    return checksum == remote_checksum;
}

void put_wrapper(char *filename) {
    int fd = open(filename, O_RDONLY);
    if(fd < 0) {
        printf("The %s doesn't exist\n", filename);
        return;
    }
    FILE *remote = fdopen(dup(client.fd), "w");
    setvbuf(remote, NULL, _IONBF, 0);
    off_t filesize = lseek(fd, 0, SEEK_END);
    fprintf(remote, "%d %s\n%lld\n", PUT, filename, filesize);
    fclose(remote);
    if(filesize == 0) {
        cerr << "zero file size\n";
        printf("put %s successfully\n", filename);
        return;
    }
    while(true) {
        int res = put(fd);
        if(res < 0) {
            close(fd);
            return;
        }
        if(res) break;
        cerr << "put failed\n";
    }
    printf("put %s successfully\n", filename);
}

void bad_format() {
    printf("Command format error\n");
}

void handle_command() {
    while(true) {
        char command[100], filename[100], line[100] = {0}, buf[100];
        fgets(line, 100, stdin);
        int count = sscanf(line, "%s%s", command, filename);
        if(count < 0) break;
        else if(!strcmp(command, "ls")) {
            count == 1 ? ls() : bad_format();
        }
        else if(!strcmp(command, "get")) {
            count == 2 && strlen(line) == sprintf(buf, "%s %s\n", command, filename) ? get_wrapper(filename) : bad_format();
        }
        else if(!strcmp(command, "put")) {
            count == 2 && strlen(line) == sprintf(buf, "%s %s\n", command, filename) ? put_wrapper(filename) : bad_format();
        }
        else printf("Command not found\n");
    }
}

void client_cleanup(int var) {
    cerr << "terminating signal received\n";
    close_connection(0);
}

int main(int argc, char **argv) {
    dir_init();
    signal(SIGINT, client_cleanup);
    signal(SIGPIPE, client_cleanup);
    int res = client_init(argc, argv);
    if(res < 0) {
        perror("connect");
        exit(1);
    }
    handle_login();
    handle_command();
    close_connection(0);
}