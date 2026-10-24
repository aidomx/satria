#include <rupa.h>
#include <stdio.h>

int main(void) {
  thisBade();
  printf("core=%d bade=%d\n", core_value(), bade_value());
  return 0;
}
/* this comment in core/src/main.c */
