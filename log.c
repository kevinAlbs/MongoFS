//
// Created by Kevin Albertson on 1/8/18.
//

#include "log.h"

#include <stdarg.h>

void init_log(const char* logpath) {
   log_file = fopen(logpath, "a");
   if (log_file == NULL) {
      fprintf(stderr, "could not open %s for logging\n", logpath);
      return;
   }
}

//void log(const char* format, ...) {
//   va_list args;
//   va_start (args, format);
//   // Not sure how to forward this to printf.
//}
void cleanup_log() {
   fclose(log_file);
}