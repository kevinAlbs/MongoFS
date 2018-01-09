//
// Created by Kevin Albertson on 1/8/18.
//

#ifndef MONGOFS_LOG_H
#define MONGOFS_LOG_H

#include <stdio.h>

FILE* log_file; // for fprintf

#define MONGOFS_LOG(...) fprintf(log_file, __VA_ARGS__)

void init_log(const char* logpath);

// void log(const char* format, ...);

void cleanup_log();

#endif //MONGOFS_LOG_H
