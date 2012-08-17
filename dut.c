#include <sys/time.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>

int test = RLIMIT_MEMLOCK;

const unsigned long GB = 1 << 30;
int NUM_GB = 55;

int main(int argc, char **argv)
{
  int ps = getpagesize();
  int i;
  volatile char *mem;
  
  if (argc == 2) {
    NUM_GB = atoi(argv[1]);
    if (NUM_GB == 0) {
      printf("Usage: dut [NUM_GB]\n");
      return -1;
    }
  }

  mem = mmap(NULL, NUM_GB * GB, PROT_READ | PROT_WRITE, MAP_POPULATE | MAP_NORESERVE | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  printf("%p\n", mem);

  if (!mem) {
    printf("mem.\n");
    return 1;
  }

  for (i = 0; i * sizeof(char) < NUM_GB * GB; i += ps)
    mem[i] = time(NULL);

  if (mem == MAP_FAILED) {
    perror("Error allocating memory");
    return -1;
  }

  printf("%d GB allocated...\n", NUM_GB);
  while (1);

}
