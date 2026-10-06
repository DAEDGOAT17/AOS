#ifndef NET_STACK_H
#define NET_STACK_H

#include <stdint.h>

// Simple socket wrapper
typedef struct {
    uint32_t dest_ip;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    int state;
} tcp_socket_t;

// AI Core Agent Client
void ollama_request(const char* ip_str, const char* prompt);
void ollama_agent_request(const char* ip_str, const char* prompt);
void ollama_feedback_request(const char* ip_str, const char* feedback);
void ollama_mock_intercept(void);

extern tcp_socket_t active_sockets[8];
void net_stack_receive(uint8_t* packet, uint32_t len);
void ping_request(const char* ip_str);

#endif
