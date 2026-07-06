#ifndef STDLIB_FIX_H
#define STDLIB_FIX_H

extern "C" {
    int at_quick_exit(void (*func)(void));
    void quick_exit(int status);
}

#endif
