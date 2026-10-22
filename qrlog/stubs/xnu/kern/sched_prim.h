#pragma once
#include <stddef.h>
typedef int wait_result_t;
typedef int kern_return_t;
typedef struct thread *thread_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
#define KERN_SUCCESS 0
