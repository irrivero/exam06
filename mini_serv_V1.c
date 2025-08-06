#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

// Client structure - stores info for each connected client
typedef struct s_client
{
	int client_id;		  // Unique ID for this client (0, 1, 2, ...)
	char *message_buffer; // Stores partial messages until we get a complete line
} t_client;

// File descriptor sets for select() - these track which sockets to monitor
fd_set ready_to_read, ready_to_write, all_sockets;

// Global server state
int server_socket = 0;	   // Main server socket that accepts new connections
int highest_fd = 0;		   // Highest file descriptor number (needed for select)
int next_available_id = 0; // Next ID to assign to new client

// Client storage - use file descriptor as array index
t_client client_array[100];

// Reusable buffers to avoid repeated malloc/free
char outgoing_message[65000], incoming_data[65000];

/**
 * Error handling - print message and exit
 * @param error_msg: Custom error message, or NULL for default "Fatal error"
 */
void fatal_error(const char *error_msg)
{
	if (error_msg)
		write(STDERR_FILENO, error_msg, strlen(error_msg));
	else
		write(STDERR_FILENO, "Fatal error\n", 12);
	exit(1);
}

/**
 * Send message to all connected clients except the sender
 * @param sender_fd: File descriptor of client who sent the message (exclude them)
 * @param message: Message to broadcast
 */
void broadcast_to_others(int sender_fd, const char *message)
{
	// Loop through all possible file descriptors up to the highest one
	for (int current_fd = 0; current_fd <= highest_fd; current_fd++)
	{
		// Check if this fd is active, not the server, and not the sender
		if (FD_ISSET(current_fd, &ready_to_write) &&
			current_fd != server_socket &&
			current_fd != sender_fd)
		{
			send(current_fd, message, strlen(message), 0);
		}
	}
}

/**
 * Clean up when a client disconnects
 * @param client_fd: File descriptor of disconnecting client
 */
void remove_client(int client_fd)
{
	FD_CLR(client_fd, &all_sockets);			// Remove from monitoring
	if (client_array[client_fd].message_buffer) // Free message buffer if exists
	{
		free(client_array[client_fd].message_buffer);
		client_array[client_fd].message_buffer = NULL;
	}
	close(client_fd); // Close the socket
}

/**
 * Extract one complete message (ending with \n) from buffer
 * @param buffer_ptr: Pointer to buffer (will be modified to remove extracted message)
 * @param message_ptr: Will point to extracted message (caller must free)
 * @return: 1 = message extracted, 0 = no complete message, -1 = error
 */
int get_complete_message(char **buffer_ptr, char **message_ptr)
{
	char *remaining_data;
	int i = 0;

	*message_ptr = NULL;	 // Initialize output
	if (*buffer_ptr == NULL) // No buffer to process
		return 0;

	while ((*buffer_ptr)[i]) // Search for newline
	{
		if ((*buffer_ptr)[i] == '\n') // Found complete message!
		{
			// Allocate space for data after the newline
			remaining_data = calloc(1, sizeof(*remaining_data) * (strlen(*buffer_ptr + i + 1) + 1));
			if (remaining_data == NULL)
				return -1; // Memory allocation failed

			// Copy remaining data (after \n) to new buffer
			strcpy(remaining_data, *buffer_ptr + i + 1);

			// Return the complete message (up to and including \n)
			*message_ptr = *buffer_ptr;
			(*message_ptr)[i + 1] = '\0'; // Null terminate after \n

			// Update buffer to contain only remaining data
			*buffer_ptr = remaining_data;
			return 1; // Successfully extracted message
		}
		i++;
	}
	return 0; // No complete message found
}

/**
 * Append new data to existing buffer, growing it as needed
 * @param old_buffer: Existing buffer (will be freed)
 * @param new_data: Data to append
 * @return: New buffer containing old + new data
 */
char *append_data(char *old_buffer, char *new_data)
{
	char *combined_buffer;
	int old_length;

	// Calculate old buffer length
	if (old_buffer == NULL)
		old_length = 0;
	else
		old_length = strlen(old_buffer);

	// Allocate space for combined data
	combined_buffer = malloc(sizeof(*combined_buffer) * (old_length + strlen(new_data) + 1));
	if (combined_buffer == NULL)
		return NULL;

	// Start with empty string
	combined_buffer[0] = '\0';

	// Copy old data if it exists
	if (old_buffer != NULL)
		strcat(combined_buffer, old_buffer);

	free(old_buffer);				   // Free old buffer
	strcat(combined_buffer, new_data); // Append new data
	return combined_buffer;
}

int main(int argc, char **argv)
{
	// Validate command line arguments
	if (argc != 2)
		fatal_error("Wrong number of arguments\n");

	// Server address configuration
	struct sockaddr_in server_address;
	socklen_t address_length = sizeof(server_address);

	// Create TCP socket
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socket < 0)
		fatal_error(NULL);

	// Configure server address structure
	bzero(&server_address, sizeof(server_address));
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = htonl(2130706433); // 127.0.0.1 in decimal
	server_address.sin_port = htons(atoi(argv[1]));		// Port from command line

	// Bind socket to address and start listening
	if (bind(server_socket, (struct sockaddr *)&server_address, address_length) < 0)
		fatal_error(NULL);
	if (listen(server_socket, 10) < 0)
		fatal_error(NULL);

	// Initialize file descriptor set - start monitoring server socket
	FD_ZERO(&all_sockets);
	FD_SET(server_socket, &all_sockets);
	highest_fd = server_socket;

	// Main server loop - never exits
	while (1)
	{
		// Copy master set to working sets (select modifies them)
		ready_to_read = ready_to_write = all_sockets;

		// Wait for activity on any socket
		if (select(highest_fd + 1, &ready_to_read, &ready_to_write, NULL, NULL) < 0)
			continue; // Select failed, try again

		// Check every possible file descriptor for activity
		for (int current_fd = 0; current_fd <= highest_fd; current_fd++)
		{
			// Skip if no activity on this fd
			if (!FD_ISSET(current_fd, &ready_to_read))
				continue;

			if (current_fd == server_socket)
			{
				// NEW CLIENT CONNECTION
				int new_client_fd = accept(server_socket,
										   (struct sockaddr *)&server_address,
										   &address_length);
				if (new_client_fd < 0)
					continue; // Accept failed, try again later

				// Add new client to monitoring
				FD_SET(new_client_fd, &all_sockets);
				if (new_client_fd > highest_fd)
					highest_fd = new_client_fd;

				// Initialize client data - use fd as array index!
				client_array[new_client_fd].client_id = next_available_id++;
				client_array[new_client_fd].message_buffer = NULL;

				// Notify all other clients about new arrival
				sprintf(outgoing_message, "server: client %d just arrived\n",
						client_array[new_client_fd].client_id);
				broadcast_to_others(new_client_fd, outgoing_message);
			}
			else
			{
				// EXISTING CLIENT SENT DATA
				ssize_t bytes_received = recv(current_fd, incoming_data,
											  sizeof(incoming_data) - 1, 0);

				if (bytes_received <= 0)
				{
					// CLIENT DISCONNECTED
					sprintf(outgoing_message, "server: client %d just left\n",
							client_array[current_fd].client_id);
					broadcast_to_others(current_fd, outgoing_message);
					remove_client(current_fd);
				}
				else
				{
					// CLIENT SENT DATA - process it
					incoming_data[bytes_received] = '\0'; // Null terminate

					// Add new data to client's message buffer
					client_array[current_fd].message_buffer =
						append_data(client_array[current_fd].message_buffer, incoming_data);

					// Extract and broadcast all complete messages
					char *complete_message = NULL;
					while (get_complete_message(&client_array[current_fd].message_buffer,
												&complete_message))
					{
						// Format message with client ID and broadcast
						sprintf(outgoing_message, "client %d: %s",
								client_array[current_fd].client_id, complete_message);
						broadcast_to_others(current_fd, outgoing_message);

						// Clean up
						free(complete_message);
						complete_message = NULL;
					}
				}
			}
		}
	}
	return 0;
}