#include <bade.h>
#include <stdio.h>

int bade_value(void) {
  return 7;
}

// Untuk melihat warning saat menjalankan parallel, apakah output realtime progress rusak?
static void sayHelloWorld() {
  printf("Hello world!\n");
}

void thisBade() {
  printf("this bade\n");
}
