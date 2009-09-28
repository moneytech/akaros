#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <fcntl.h>

#define SYSCALL_SERVER_PTY ".syscall_server_pty"

int init_syscall_server(int* fd_read, int* fd_write) {
	// File descriptor of our open serial port
	
	int fd = posix_openpt(O_RDWR | O_NOCTTY);
	if(fd < 0)
		return fd;
	grantpt (fd);
    unlockpt (fd);
	char* pty_dev = ptsname(fd);
	
	//Output the newly allocated slave device into a file
	int pty_fd = open(SYSCALL_SERVER_PTY, 
	                  O_RDWR | O_CREAT | O_TRUNC, 
	                  S_IRUSR | S_IWUSR);
	write(pty_fd, pty_dev, strnlen(pty_dev));
	*fd_read = *fd_write = fd;
	return fd;
}
