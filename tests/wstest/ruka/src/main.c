#include <stdio.h>
#include <string.h>

#if __has_include("build/embedded.h")
#include "build/embedded.h"
extern const unsigned char EMBED_MODULES_SYMBOL[];
extern const unsigned char EMBED_MODULES_SYMBOL_END[];
#define RUPA_EMBEDDED_START EMBED_MODULES_SYMBOL
#define RUPA_EMBEDDED_END EMBED_MODULES_SYMBOL_END
#define RUPA_EMBEDDED_ARCHIVE_NAME EMBED_MODULES_ARCHIVE_NAME
#define RUPA_EMBEDDED_EXTRACT_DIR EMBED_MODULES_EXTRACT_DIR
#else
#define RUPA_EMBEDDED_ARCHIVE_NAME "rupa_modules.tar"
#define RUPA_EMBEDDED_EXTRACT_DIR "/tmp/rupa-system"
extern const unsigned char _binary_modules_rupa_modules_tar_gz_start[] __attribute__((weak));
extern const unsigned char _binary_modules_rupa_modules_tar_gz_end[] __attribute__((weak));
#define RUPA_EMBEDDED_START _binary_modules_rupa_modules_tar_gz_start
#define RUPA_EMBEDDED_END _binary_modules_rupa_modules_tar_gz_end
#endif

extern void rupa_hello();

int main(void) {
  rupa_hello();
  printf("ruka ok,archive_name=%s, modules=%zu bytes\n", RUPA_EMBEDDED_ARCHIVE_NAME,
         (size_t)RUPA_EMBEDDED_END);
  return 0;
}
