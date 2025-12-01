#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>

#define PORT 2121
#define BUFFER_SIZE 4096
#define MAX_PATH 512

typedef struct {
    int ctrl_sock;
    int data_sock;
    int passive_sock;
    char current_dir[MAX_PATH];
    char rename_from[MAX_PATH];
    int is_authenticated;
    struct sockaddr_in data_addr;
} client_session_t;

void send_response(int sock, const char *response) {
    send(sock, response, strlen(response), 0);
    printf("-> %s", response);
}

void handle_user(client_session_t *session, const char *username) {
    send_response(session->ctrl_sock, "331 Username OK, need password.\r\n");
}

void handle_pass(client_session_t *session, const char *password) {
    // Simple authentication (in production, use proper auth)
    session->is_authenticated = 1;
    send_response(session->ctrl_sock, "230 User logged in.\r\n");
}

void handle_pwd(client_session_t *session) {
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response), "257 \"%s\" is current directory.\r\n", 
             session->current_dir);
    send_response(session->ctrl_sock, response);
}

void handle_cwd(client_session_t *session, const char *path) {
    char new_path[MAX_PATH];
    
    if (path[0] == '/') {
        snprintf(new_path, sizeof(new_path), "%s", path);
    } else {
        snprintf(new_path, sizeof(new_path), "%s/%s", 
                 session->current_dir, path);
    }
    
    DIR *dir = opendir(new_path);
    if (dir) {
        closedir(dir);
        strncpy(session->current_dir, new_path, MAX_PATH - 1);
        send_response(session->ctrl_sock, "250 Directory changed.\r\n");
    } else {
        send_response(session->ctrl_sock, "550 Failed to change directory.\r\n");
    }
}

void handle_cdup(client_session_t *session) {
    char *last_slash = strrchr(session->current_dir, '/');
    if (last_slash && last_slash != session->current_dir) {
        *last_slash = '\0';
        send_response(session->ctrl_sock, "250 Directory changed.\r\n");
    } else {
        send_response(session->ctrl_sock, "550 Already at root.\r\n");
    }
}

void handle_mkd(client_session_t *session, const char *dirname) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", session->current_dir, dirname);
    
    if (mkdir(path, 0755) == 0) {
        char response[BUFFER_SIZE];
        snprintf(response, sizeof(response), "257 \"%s\" created.\r\n", dirname);
        send_response(session->ctrl_sock, response);
    } else {
        send_response(session->ctrl_sock, "550 Create directory failed.\r\n");
    }
}

void handle_rmd(client_session_t *session, const char *dirname) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", session->current_dir, dirname);
    
    if (rmdir(path) == 0) {
        send_response(session->ctrl_sock, "250 Directory removed.\r\n");
    } else {
        send_response(session->ctrl_sock, "550 Remove directory failed.\r\n");
    }
}

void handle_dele(client_session_t *session, const char *filename) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", session->current_dir, filename);
    
    if (unlink(path) == 0) {
        send_response(session->ctrl_sock, "250 File deleted.\r\n");
    } else {
        send_response(session->ctrl_sock, "550 Delete failed.\r\n");
    }
}

void handle_rnfr(client_session_t *session, const char *filename) {
    snprintf(session->rename_from, sizeof(session->rename_from), 
             "%s/%s", session->current_dir, filename);
    send_response(session->ctrl_sock, "350 Ready for RNTO.\r\n");
}

void handle_rnto(client_session_t *session, const char *filename) {
    char to_path[MAX_PATH];
    snprintf(to_path, sizeof(to_path), "%s/%s", session->current_dir, filename);
    
    if (rename(session->rename_from, to_path) == 0) {
        send_response(session->ctrl_sock, "250 Rename successful.\r\n");
    } else {
        send_response(session->ctrl_sock, "550 Rename failed.\r\n");
    }
    session->rename_from[0] = '\0';
}

void handle_type(client_session_t *session, const char *type) {
    if (type[0] == 'I' || type[0] == 'A') {
        send_response(session->ctrl_sock, "200 Type set.\r\n");
    } else {
        send_response(session->ctrl_sock, "504 Type not supported.\r\n");
    }
}

void handle_pasv(client_session_t *session) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    
    if (session->passive_sock > 0) {
        close(session->passive_sock);
    }
    
    session->passive_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (session->passive_sock < 0) {
        send_response(session->ctrl_sock, "425 Cannot open passive connection.\r\n");
        return;
    }
    
    int opt = 1;
    setsockopt(session->passive_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    
    if (bind(session->passive_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        send_response(session->ctrl_sock, "425 Cannot bind passive socket.\r\n");
        close(session->passive_sock);
        session->passive_sock = -1;
        return;
    }
    
    listen(session->passive_sock, 1);
    getsockname(session->passive_sock, (struct sockaddr*)&addr, &addr_len);
    
    unsigned char *ip = (unsigned char*)&addr.sin_addr.s_addr;
    unsigned short port = ntohs(addr.sin_port);
    
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response), 
             "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).\r\n",
             ip[0], ip[1], ip[2], ip[3], port >> 8, port & 0xFF);
    send_response(session->ctrl_sock, response);
}

int accept_data_connection(client_session_t *session) {
    if (session->passive_sock > 0) {
        session->data_sock = accept(session->passive_sock, NULL, NULL);
        close(session->passive_sock);
        session->passive_sock = -1;
        return session->data_sock;
    }
    return -1;
}

void handle_list(client_session_t *session, const char *path) {
    send_response(session->ctrl_sock, "150 Opening data connection.\r\n");
    
    int data_sock = accept_data_connection(session);
    if (data_sock < 0) {
        send_response(session->ctrl_sock, "425 Cannot open data connection.\r\n");
        return;
    }
    
    DIR *dir = opendir(session->current_dir);
    if (!dir) {
        send_response(session->ctrl_sock, "550 Cannot open directory.\r\n");
        close(data_sock);
        return;
    }
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", 
                 session->current_dir, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) == 0) {
            char listing[BUFFER_SIZE];
            char perms[11];
            
            snprintf(perms, sizeof(perms), "%c%c%c%c%c%c%c%c%c%c",
                     S_ISDIR(st.st_mode) ? 'd' : '-',
                     (st.st_mode & S_IRUSR) ? 'r' : '-',
                     (st.st_mode & S_IWUSR) ? 'w' : '-',
                     (st.st_mode & S_IXUSR) ? 'x' : '-',
                     (st.st_mode & S_IRGRP) ? 'r' : '-',
                     (st.st_mode & S_IWGRP) ? 'w' : '-',
                     (st.st_mode & S_IXGRP) ? 'x' : '-',
                     (st.st_mode & S_IROTH) ? 'r' : '-',
                     (st.st_mode & S_IWOTH) ? 'w' : '-',
                     (st.st_mode & S_IXOTH) ? 'x' : '-');
            
            snprintf(listing, sizeof(listing), "%s %3ld %8ld %s\r\n",
                     perms, (long)st.st_nlink, (long)st.st_size, entry->d_name);
            send(data_sock, listing, strlen(listing), 0);
        }
    }
    
    closedir(dir);
    close(data_sock);
    send_response(session->ctrl_sock, "226 Transfer complete.\r\n");
}

void handle_retr(client_session_t *session, const char *filename) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", session->current_dir, filename);
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        send_response(session->ctrl_sock, "550 File not found.\r\n");
        return;
    }
    
    send_response(session->ctrl_sock, "150 Opening data connection.\r\n");
    
    int data_sock = accept_data_connection(session);
    if (data_sock < 0) {
        send_response(session->ctrl_sock, "425 Cannot open data connection.\r\n");
        close(fd);
        return;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        send(data_sock, buffer, bytes_read, 0);
    }
    
    close(fd);
    close(data_sock);
    send_response(session->ctrl_sock, "226 Transfer complete.\r\n");
}

void handle_stor(client_session_t *session, const char *filename) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", session->current_dir, filename);
    
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        send_response(session->ctrl_sock, "550 Cannot create file.\r\n");
        return;
    }
    
    send_response(session->ctrl_sock, "150 Opening data connection.\r\n");
    
    int data_sock = accept_data_connection(session);
    if (data_sock < 0) {
        send_response(session->ctrl_sock, "425 Cannot open data connection.\r\n");
        close(fd);
        return;
    }
    
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
        write(fd, buffer, bytes_read);
    }
    
    close(fd);
    close(data_sock);
    send_response(session->ctrl_sock, "226 Transfer complete.\r\n");
}

void handle_syst(client_session_t *session) {
    send_response(session->ctrl_sock, "215 UNIX Type: L8\r\n");
}

void handle_noop(client_session_t *session) {
    send_response(session->ctrl_sock, "200 OK\r\n");
}

void handle_quit(client_session_t *session) {
    send_response(session->ctrl_sock, "221 Goodbye.\r\n");
}

void process_command(client_session_t *session, char *command) {
    char *cmd = strtok(command, " \r\n");
    char *arg = strtok(NULL, "\r\n");
    
    if (!cmd) return;
    
    printf("<- %s %s\n", cmd, arg ? arg : "");
    
    if (strcasecmp(cmd, "USER") == 0) {
        handle_user(session, arg);
    } else if (strcasecmp(cmd, "PASS") == 0) {
        handle_pass(session, arg);
    } else if (!session->is_authenticated) {
        send_response(session->ctrl_sock, "530 Please login first.\r\n");
    } else if (strcasecmp(cmd, "PWD") == 0 || strcasecmp(cmd, "XPWD") == 0) {
        handle_pwd(session);
    } else if (strcasecmp(cmd, "CWD") == 0) {
        handle_cwd(session, arg);
    } else if (strcasecmp(cmd, "CDUP") == 0) {
        handle_cdup(session);
    } else if (strcasecmp(cmd, "MKD") == 0 || strcasecmp(cmd, "XMKD") == 0) {
        handle_mkd(session, arg);
    } else if (strcasecmp(cmd, "RMD") == 0 || strcasecmp(cmd, "XRMD") == 0) {
        handle_rmd(session, arg);
    } else if (strcasecmp(cmd, "DELE") == 0) {
        handle_dele(session, arg);
    } else if (strcasecmp(cmd, "RNFR") == 0) {
        handle_rnfr(session, arg);
    } else if (strcasecmp(cmd, "RNTO") == 0) {
        handle_rnto(session, arg);
    } else if (strcasecmp(cmd, "TYPE") == 0) {
        handle_type(session, arg);
    } else if (strcasecmp(cmd, "PASV") == 0) {
        handle_pasv(session);
    } else if (strcasecmp(cmd, "LIST") == 0) {
        handle_list(session, arg);
    } else if (strcasecmp(cmd, "RETR") == 0) {
        handle_retr(session, arg);
    } else if (strcasecmp(cmd, "STOR") == 0) {
        handle_stor(session, arg);
    } else if (strcasecmp(cmd, "SYST") == 0) {
        handle_syst(session);
    } else if (strcasecmp(cmd, "NOOP") == 0) {
        handle_noop(session);
    } else if (strcasecmp(cmd, "QUIT") == 0) {
        handle_quit(session);
    } else {
        send_response(session->ctrl_sock, "502 Command not implemented.\r\n");
    }
}

void *handle_client(void *arg) {
    client_session_t *session = (client_session_t*)arg;
    char buffer[BUFFER_SIZE];
    
    send_response(session->ctrl_sock, "220 Custom FTP Server Ready.\r\n");
    
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = recv(session->ctrl_sock, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_read <= 0) break;
        
        process_command(session, buffer);
        
        if (strncasecmp(buffer, "QUIT", 4) == 0) break;
    }
    
    if (session->passive_sock > 0) close(session->passive_sock);
    if (session->data_sock > 0) close(session->data_sock);
    close(session->ctrl_sock);
    free(session);
    
    printf("Client disconnected.\n");
    return NULL;
}

int main() {
    int server_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(1);
    }
    
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(1);
    }
    
    if (listen(server_sock, 10) < 0) {
        perror("Listen failed");
        exit(1);
    }
    
    printf("FTP Server listening on port %d...\n", PORT);
    
    while (1) {
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock < 0) {
            perror("Accept failed");
            continue;
        }
        
        printf("Client connected from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        client_session_t *session = malloc(sizeof(client_session_t));
        memset(session, 0, sizeof(client_session_t));
        session->ctrl_sock = client_sock;
        session->data_sock = -1;
        session->passive_sock = -1;
        getcwd(session->current_dir, sizeof(session->current_dir));
        
        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, session);
        pthread_detach(thread);
    }
    
    close(server_sock);
    return 0;
}
