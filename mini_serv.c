#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct s_client
{
	int fd;
	int id;
	char *msg;
	struct s_client *next;
} t_client;

t_client *clients = NULL;
int next_id = 0;
fd_set read_fds, write_fds, master_fds;
int max_fd;
int server_fd;

void fatal_error()
{
	write(STDERR_FILENO, "Fatal error\n", 12);
	exit(1);
}

void *safe_malloc(size_t size)
{
	void *ptr = malloc(size);
	if (!ptr)
		fatal_error();
	return ptr;
}

void *safe_realloc(void *ptr, size_t size)
{
	void *new_ptr = realloc(ptr, size);
	if (!new_ptr)
		fatal_error();
	return new_ptr;
}

void add_client(int fd)
{
	t_client *new_client = safe_malloc(sizeof(t_client));
	new_client->fd = fd;
	new_client->id = next_id++;
	new_client->msg = safe_malloc(1);
	new_client->msg[0] = '\0';
	new_client->next = clients;
	clients = new_client;

	if (fd > max_fd)
		max_fd = fd;
	FD_SET(fd, &master_fds);
}

void remove_client(int fd)
{
	t_client **current = &clients;

	while (*current)
	{
		if ((*current)->fd == fd)
		{
			t_client *to_remove = *current;
			*current = (*current)->next;
			free(to_remove->msg);
			free(to_remove);
			break;
		}
		current = &(*current)->next;
	}

	FD_CLR(fd, &master_fds);
	close(fd);
}

t_client *find_client(int fd)
{
	t_client *current = clients;
	while (current)
	{
		if (current->fd == fd)
			return current;
		current = current->next;
	}
	return NULL;
}

void send_to_all_except(int sender_fd, char *msg)
{
	t_client *current = clients;
	while (current)
	{
		if (current->fd != sender_fd)
		{
			send(current->fd, msg, strlen(msg), 0);
		}
		current = current->next;
	}
}

void send_to_all(char *msg)
{
	send_to_all_except(-1, msg);
}

t_client *find_client_by_id(int id)
{
	t_client *current = clients;
	while (current)
	{
		if (current->id == id)
			return current;
		current = current->next;
	}
	return NULL;
}

void notify_arrival(int client_id)
{
	char msg[100];
	sprintf(msg, "server: client %d just arrived\n", client_id);
	send_to_all_except(find_client_by_id(client_id)->fd, msg);
}

void notify_departure(int client_id)
{
	char msg[100];
	sprintf(msg, "server: client %d just left\n", client_id);
	send_to_all(msg);
}

void handle_client_message(t_client *client)
{
	char buffer[1000];
	int bytes_read = recv(client->fd, buffer, sizeof(buffer) - 1, 0);

	if (bytes_read <= 0)
	{
		notify_departure(client->id);
		remove_client(client->fd);
		return;
	}

	buffer[bytes_read] = '\0';

	// Append to client message buffer
	int old_len = strlen(client->msg);
	client->msg = safe_realloc(client->msg, old_len + bytes_read + 1);
	strcat(client->msg, buffer);

	// Process complete lines
	char *line_start = client->msg;
	char *newline;

	while ((newline = strstr(line_start, "\n")) != NULL)
	{
		*newline = '\0';

		// Send line to all other clients
		char msg[2000];
		sprintf(msg, "client %d: %s\n", client->id, line_start);
		send_to_all_except(client->fd, msg);

		line_start = newline + 1;
	}

	// Keep remaining incomplete line
	if (strlen(line_start) > 0)
	{
		int remaining_len = strlen(line_start);
		char *temp = safe_malloc(remaining_len + 1);
		strcpy(temp, line_start);
		free(client->msg);
		client->msg = temp;
	}
	else
	{
		free(client->msg);
		client->msg = safe_malloc(1);
		client->msg[0] = '\0';
	}
}

void accept_new_client()
{
	struct sockaddr_in client_addr;
	socklen_t addr_len = sizeof(client_addr);
	int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);

	if (client_fd == -1)
		return;

	add_client(client_fd);
	t_client *new_client = find_client(client_fd);
	notify_arrival(new_client->id);
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		write(STDERR_FILENO, "Wrong number of arguments\n", 26);
		exit(1);
	}

	int port = atoi(argv[1]);

	// Create socket
	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1)
		fatal_error();

	// Set socket options
	int opt = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
		fatal_error();

	// Bind socket
	struct sockaddr_in server_addr;
	bzero(&server_addr, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
	server_addr.sin_port = htons(port);

	if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
		fatal_error();

	// Listen
	if (listen(server_fd, 10) == -1)
		fatal_error();

	// Initialize fd sets
	FD_ZERO(&master_fds);
	FD_SET(server_fd, &master_fds);
	max_fd = server_fd;

	// Main loop
	while (1)
	{
		read_fds = master_fds;

		if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) == -1)
			continue;

		// Check for new connections
		if (FD_ISSET(server_fd, &read_fds))
		{
			accept_new_client();
		}

		// Check existing clients
		t_client *current = clients;
		while (current)
		{
			t_client *next = current->next; // Save next before potential removal
			if (FD_ISSET(current->fd, &read_fds))
			{
				handle_client_message(current);
			}
			current = next;
		}
	}

	return 0;
}