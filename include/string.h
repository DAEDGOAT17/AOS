#ifndef STRING_H
#define STRING_H

#include <stddef.h>

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strncasecmp(const char *s1, const char *s2, size_t n);
size_t strlen(const char *s);
char *strstr(const char *haystack, const char *needle);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
char* strcat(char* dest, const char* src);
char* strchr(const char* s, int c);

void* memset(void* bufptr, int value, size_t size);
void* memcpy(void* dstptr, const void* srcptr, size_t size);
void* memmove(void* dstptr, const void* srcptr, size_t size);
int memcmp(const void* s1, const void* s2, size_t n);

#endif