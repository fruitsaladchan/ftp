# ftp
simple FTP server implementation following RFC specifications. Supports multiple concurrent connections and implements the core FTP command set.


### to compile and run
```
gcc -o ftp ftp.c -lpthread
./ftp
```

### test with ftp client
```
ftp localhost 2121
# try commands like: ls, cd, mkdir, get, put, etc.
# login with any username/password
````

### core commands

- USER/PASS - Authentication
- PWD/CWD/CDUP - Directory navigation
- MKD/RMD - Create/remove directories
- LIST - Directory listing
- RETR/STOR - File download/upload
- DELE - Delete files
- RNFR/RNTO - Rename files
- TYPE - Set transfer type
- PASV - Passive mode for data connections
- SYST/NOOP/QUIT - System commands

### features
- Multi-threaded support for concurrent connections using pthreads
- Passive mode data transfers (PASV)
- Unix-style directory listings
- Session management per client
- Proper error handling and response codes
