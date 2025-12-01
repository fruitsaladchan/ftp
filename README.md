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
