#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>

int main(int argc, char* argv[])
{
	// Initalize logger
	openlog("writer", LOG_PID | LOG_CONS, LOG_USER);

	// Enough arguments?
	if (argc < 3)
	{
		const char* usage = "Usage: ./writer [FILE] [STRING]\n";
		write(2, usage, strlen(usage));
		syslog(LOG_ERR, usage);
		closelog();
		return 1;
	}

	syslog(LOG_DEBUG, "Writing %s to %s", argv[2], argv[1]);

	// Open an existing file or create it
	int fd = open(argv[1], O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0)
	{
		const char* err = "Error: failed to open or create file\n";
		write(2, err, strlen(err));
		syslog(LOG_ERR, err);
		closelog();
		return 1;
	}
	// Write the string to the file
	write(fd, argv[2], strlen(argv[2]));
	//Cleanup
	close(fd);
	closelog();
	return 0;
}

