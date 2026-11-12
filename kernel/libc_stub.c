/*
 * Minimal C library stubs for freestanding kernel code
 */

#include <string.h>
#include <math.h>
#include <stdarg.h>

/* Simple strcpy implementation */
char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++))
        ;
    return dest;
}

/* Simple strrchr implementation */
char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if ((char)c == '\0')
        return (char *)s;
    return (char *)last;
}

/* Simple sin function for animations (using Taylor series approximation) */
double sin(double x)
{
    /* Normalize to [-pi, pi] */
    while (x > 3.14159265358979323846)
        x -= 2 * 3.14159265358979323846;
    while (x < -3.14159265358979323846)
        x += 2 * 3.14159265358979323846;

    /* Taylor series: sin(x) = x - x^3/3! + x^5/5! - x^7/7! + ... */
    double result = x;
    double term = x;
    for (int i = 1; i < 10; i++) {
        term *= -x * x / ((2 * i) * (2 * i + 1));
        result += term;
    }
    return result;
}

/* Simple cos function */
double cos(double x)
{
    /* cos(x) = sin(x + pi/2) */
    return sin(x + 1.5707963267948966);
}

/* Simple sqrt function */
double sqrt(double x)
{
    if (x < 0.0)
        return -1.0;
    if (x == 0.0)
        return 0.0;

    /* Newton's method */
    double guess = x;
    for (int i = 0; i < 20; i++) {
        double next = (guess + x / guess) / 2.0;
        if (next == guess)
            break;
        guess = next;
    }
    return guess;
}

/* Minimal sscanf for basic parsing */
int __isoc23_sscanf(const char *str, const char *fmt, ...)
{
    /* Very basic implementation for the specific use case in file_explorer.c */
    /* Format: "%127[^|]|%15[^|]|%d" to parse "name|type|size" */
    va_list args;
    va_start(args, fmt);

    int count = 0;
    const char *p = str;
    const char *f = fmt;

    while (*f && *p) {
        if (*f == '%') {
            f++;
            if (*f == 'd') {
                /* Parse integer */
                int *pi = va_arg(args, int *);
                *pi = 0;
                while (*p >= '0' && *p <= '9') {
                    *pi = *pi * 10 + (*p - '0');
                    p++;
                }
                count++;
            } else if (*f == 's' || (*f >= '0' && *f <= '9')) {
                /* Parse string (simplified) */
                char *buf = va_arg(args, char *);
                size_t maxlen = 256;

                if (*f >= '0' && *f <= '9') {
                    /* Parse format like "127[^|]" */
                    f++;
                    if (*f == '[') {
                        f++;
                        char stopchar = '^' == *f ? (f++, *f++) : '\0';
                        while (stopchar && *p && *p != stopchar && maxlen > 1) {
                            *buf++ = *p++;
                            maxlen--;
                        }
                        *buf = '\0';
                        count++;
                        if (stopchar && *p == stopchar)
                            p++;
                        f++; /* skip ] */
                    }
                } else {
                    while (*p && *p != ' ' && maxlen > 1) {
                        *buf++ = *p++;
                        maxlen--;
                    }
                    *buf = '\0';
                    count++;
                }
            }
            f++;
        } else if (*f == '|' || *f == ' ') {
            /* Skip delimiters */
            while (*p && (*p == *f || (*f == '|' && (*p == '|' || *p == ' '))))
                p++;
            f++;
        } else {
            f++;
            p++;
        }
    }

    va_end(args);
    return count;
}

