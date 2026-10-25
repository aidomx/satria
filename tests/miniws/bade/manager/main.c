#include <bade.h>
#include <stdio.h>

int main(void) {
  thisCore();
  badeExtractSystem();
  printf("bade=%d core=%d\n", bade_value(), core_value());
  return 0;
}
