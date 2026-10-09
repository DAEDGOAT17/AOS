#ifndef SHELL_H
#define SHELL_H

#define SHELL_KEY_UP 0x1C
#define SHELL_KEY_DOWN 0x1D

void shell_input(char c);
void shell_task(void);
void shell_queue_text(const char* text);
int shell_install_runtime_command(const char* name);
int shell_install_lisp_command(const char* definition);
int shell_queue_lisp_app(const char* name);
int shell_queue_lisp_expression(const char* expression);

#endif