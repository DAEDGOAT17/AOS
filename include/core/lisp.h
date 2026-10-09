#ifndef CORE_LISP_H
#define CORE_LISP_H

int aos_lisp_validate(const char* source);
int aos_lisp_execute(const char* source, const char* argument);

#endif