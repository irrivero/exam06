#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define MAX_CLIENTS 100
#define BUFFER_SIZE 65000
#define LOCALHOST_IP 2130706433 // 127.0.0.1 in decimal

typedef struct s_client
{
	int client_id;
	char *message_buffer;
} t_client;

// Global state
fd_set read_set, write_set, master_set;
int server_socket = 0, highest_fd = 0, next_client_id = 0;
t_client client_list[MAX_CLIENTS];

char send_buffer[BUFFER_SIZE], receive_buffer[BUFFER_SIZE];

// ============================================================================
// ERROR HANDLING
// ============================================================================

void fatal_error(const char *error_message)
{
	if (error_message)
		write(STDERR_FILENO, error_message, strlen(error_message));
	else
		write(STDERR_FILENO, "Fatal error\n", 12);
	exit(1);
}

// ============================================================================
// MESSAGE BROADCASTING
// ============================================================================

void broadcast_to_all_except(int sender_fd, const char *message)
{
	for (int fd = 0; fd <= highest_fd; fd++)
	{
		if (FD_ISSET(fd, &write_set) && fd != server_socket && fd != sender_fd)
		{
			send(fd, message, strlen(message), 0);
		}
	}
}

void notify_client_arrival(int new_client_fd)
{
	int client_id = client_list[new_client_fd].client_id;
	sprintf(send_buffer, "server: client %d just arrived\n", client_id);
	broadcast_to_all_except(new_client_fd, send_buffer);
}

void notify_client_departure(int departed_client_fd)
{
	int client_id = client_list[departed_client_fd].client_id;
	sprintf(send_buffer, "server: client %d just left\n", client_id);
	broadcast_to_all_except(departed_client_fd, send_buffer);
}

// ============================================================================
// CLIENT MANAGEMENT
// ============================================================================

void initialize_new_client(int client_fd)
{
	client_list[client_fd].client_id = next_client_id++;
	client_list[client_fd].message_buffer = NULL;
}

void cleanup_client(int client_fd)
{
	FD_CLR(client_fd, &master_set);
	if (client_list[client_fd].message_buffer)
	{
		free(client_list[client_fd].message_buffer);
		client_list[client_fd].message_buffer = NULL;
	}
	close(client_fd);
}

// ============================================================================
// MESSAGE PROCESSING
// ============================================================================

char *append_to_buffer(char *existing_buffer, char *new_data)
{
	char *combined_buffer;
	int existing_length;

	if (existing_buffer == NULL)
		existing_length = 0;
	else
		existing_length = strlen(existing_buffer);

	combined_buffer = malloc(sizeof(*combined_buffer) * (existing_length + strlen(new_data) + 1));
	if (combined_buffer == NULL)
		return NULL;

	combined_buffer[0] = '\0';
	if (existing_buffer != NULL)
		strcat(combined_buffer, existing_buffer);

	free(existing_buffer); // Free old buffer
	strcat(combined_buffer, new_data);
	return combined_buffer;
}

int extract_complete_message(char **buffer, char **extracted_message)
{
	char *remaining_buffer;
	int position;

	*extracted_message = NULL;
	if (*buffer == NULL)
		return 0; // No buffer to process

	position = 0;
	while ((*buffer)[position])
	{
		if ((*buffer)[position] == '\n') // Found complete message
		{
			// Allocate space for remaining data after newline
			remaining_buffer = calloc(1, sizeof(*remaining_buffer) * (strlen(*buffer + position + 1) + 1));
			if (remaining_buffer == NULL)
				return -1; // Memory allocation failed

			strcpy(remaining_buffer, *buffer + position + 1); // Copy remaining data

			*extracted_message = *buffer;			   // Return complete message
			(*extracted_message)[position + 1] = '\0'; // Null-terminate message (include \n)

			*buffer = remaining_buffer; // Update buffer with remaining data
			return 1;					// Successfully extracted message
		}
		position++;
	}
	return 0; // No complete message found
}

void broadcast_client_message(int sender_fd, char *message)
{
	int client_id = client_list[sender_fd].client_id;
	sprintf(send_buffer, "client %d: %s", client_id, message);
	broadcast_to_all_except(sender_fd, send_buffer);
}

// ============================================================================
// CONNECTION HANDLING
// ============================================================================

void handle_new_connection(void)
{
	struct sockaddr_in client_address;
	socklen_t address_length = sizeof(client_address);
	int new_client_fd;

	new_client_fd = accept(server_socket, (struct sockaddr *)&client_address, &address_length);
	if (new_client_fd < 0)
		return; // Accept failed, but don't crash

	// Add client to monitoring
	FD_SET(new_client_fd, &master_set);
	if (new_client_fd > highest_fd)
		highest_fd = new_client_fd;

	// Initialize client data
	initialize_new_client(new_client_fd);

	// Notify other clients
	notify_client_arrival(new_client_fd);
}

void handle_client_message(int client_fd)
{
	ssize_t bytes_received;
	char *extracted_message = NULL;

	bytes_received = recv(client_fd, receive_buffer, sizeof(receive_buffer) - 1, 0);

	if (bytes_received <= 0) // Client disconnected
	{
		notify_client_departure(client_fd);
		cleanup_client(client_fd);
		return;
	}

	// Process received data
	receive_buffer[bytes_received] = '\0';
	client_list[client_fd].message_buffer = append_to_buffer(client_list[client_fd].message_buffer, receive_buffer);

	// Extract and broadcast all complete messages
	while (extract_complete_message(&client_list[client_fd].message_buffer, &extracted_message))
	{
		broadcast_client_message(client_fd, extracted_message);
		free(extracted_message);
		extracted_message = NULL;
	}
}

// ============================================================================
// SERVER SETUP
// ============================================================================

void setup_server_socket(int port)
{
	struct sockaddr_in server_address;
	socklen_t address_length = sizeof(server_address);

	// Create socket
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket < 0)
		fatal_error(NULL);

	// Configure server address
	bzero(&server_address, sizeof(server_address));
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = htonl(LOCALHOST_IP); // 127.0.0.1
	server_address.sin_port = htons(port);

	// Bind and listen
	if (bind(server_socket, (struct sockaddr *)&server_address, address_length) < 0)
		fatal_error(NULL);
	if (listen(server_socket, 10) < 0)
		fatal_error(NULL);

	// Initialize file descriptor sets
	FD_ZERO(&master_set);
	FD_SET(server_socket, &master_set);
	highest_fd = server_socket;
}

// ============================================================================
// MAIN PROGRAM
// ============================================================================

int main(int argc, char **argv)
{
	if (argc != 2)
		fatal_error("Wrong number of arguments\n");

	int port = atoi(argv[1]);
	setup_server_socket(port);

	// Main event loop
	while (1)
	{
		// Prepare file descriptor sets for select()
		read_set = write_set = master_set;

		if (select(highest_fd + 1, &read_set, &write_set, NULL, NULL) < 0)
			continue; // Select failed, try again

		// Check all possible file descriptors
		for (int fd = 0; fd <= highest_fd; fd++)
		{
			if (!FD_ISSET(fd, &read_set))
				continue; // No activity on this fd

			if (fd == server_socket)
			{
				handle_new_connection();
			}
			else
			{
				handle_client_message(fd);
			}
		}
	}

	return 0;
}