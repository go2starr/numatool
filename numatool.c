/*
 * numatool.c
 */ 

#include <syscall.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>

#define NUMA_MAX_NODES 2
#define MAX_PROCS 10
#define MAX_MV_PAGES 1000

#define MIN_BUF_POOL_GB 1
#define GB (1 << 30)


/*
 * move_pages - syscall wrapper
 */
long move_pages(int pid, unsigned long count,
                void **pages, const int *nodes, int *status, int flags)
{
  return syscall(__NR_move_pages, pid, count, pages, nodes, status, flags);
}

/*
 * get_largest_anon_set - return @pid's largest anonymous segment of memory
 */
int get_largest_anon_seg(int pid, void **va, unsigned long *ct)
{
  size_t len = 0;
  char *line = NULL;
  char fn[64];
  FILE *f;
  unsigned long long size;
  int ps = getpagesize();

  *ct = 0;

  sprintf(fn, "/proc/%d/numa_maps", pid);
  f = fopen(fn, "r");
  if (!f)
    return -1;
  while (getdelim(&line, &len, '\n', f) > 0) {
    char *end;
    char *s = strstr(line, "anon");
    if (!s)
      continue;
    while (!isdigit(*s))
      ++s;
    size = strtoull(s, &end, 0);
    if (end == s)
      size = 0;
    if (size > *ct) {
      *ct = size;
      *va = (void *)strtoull(line, NULL, 16);
    }
  }
  if (*ct == 0)
    return -1;
  return 0;
}

/* 
 * get_free_count - get number of free bytes on a node
 */
long long get_free_count(int node)
{
  size_t len = 0;
  char *line = NULL;
  char fn[64];
  long long size = -1;
  FILE *f;

  sprintf(fn, "/sys/devices/system/node/node%d/meminfo", node);
  f = fopen(fn, "r");
  if (!f)
    return -1;
  while (getdelim(&line, &len, '\n', f) > 0) {
    char *end;
    char *s = strstr(line, "kB");
    if (!s)
      continue;
    --s;
    while (s > line && isspace(*s))
      --s;
    while (s > line && isdigit(*s))
      --s;
    if (strstr(line, "MemFree")) {
      size = strtoull(s, &end, 0) << 10;
      if (end == s)
        size = -1;
    }
  }
  fclose(f);
  free(line);
  return size;
}
      

/* 
 * set_configured_nodes - Set the number and mask of active nodes. 
 * 
 * Note: Based on libnuma.c from numactl
 */
void set_configured_nodes(int *nr_nodes, int *mask)
{
  DIR *d;
  struct dirent *de;

  *nr_nodes = 0;
  *mask = 0;

  d = opendir("/sys/devices/system/node");
  if (!d) {
    return;
  } else {
    while ((de = readdir(d)) != NULL) {
      int nd;
      if (strncmp(de->d_name, "node", 4))
        continue;
      nd = strtoul(de->d_name+4, NULL, 0);
      *mask |= 1 << nd;
      (*nr_nodes)++;
    }
    closedir(d);
  }
}

int err() 
{
    printf("Usage: numatool PID [PIDs]...\n");
    return 1;
}

int main(int argc, char **argv)
{
  /* Syscall */
  pid_t pid[MAX_PROCS];
  void *va_start[MAX_PROCS];
  unsigned long page_count[MAX_PROCS];
  unsigned long valid_page_count[MAX_PROCS];
  void **addr[MAX_PROCS];
  int *status[MAX_PROCS];
  int *nodes[MAX_PROCS];

  /* Numa */
  int node_mask = 0;
  int max_nr_nodes;
  int node_page_count[MAX_PROCS][NUMA_MAX_NODES];
  unsigned long long free_count[NUMA_MAX_NODES];
  int target_node;
  int nr_to_swap;

  int nr_procs;
  long pagesize = getpagesize();
  long rc;
  int i, j;

  /************************************************************
   * CLA
   ************************************************************/
  if (argc < 2)
    return err();

  nr_procs = argc - 1;
  if (nr_procs > MAX_PROCS) {
    printf("Max supported procs is %d\n", MAX_PROCS);
    return -1;
  }

  for (i = 0; i < nr_procs; i++) {
    pid[i] = atoi(argv[i + 1]);
    if (!pid)
      return err();

    rc = get_largest_anon_seg(pid[i], &va_start[i], &page_count[i]);
    if (rc < 0)
      return err();
    
    if (page_count[i] < MIN_BUF_POOL_GB * (GB / pagesize)) {
      printf("Largest anon mem region size too small, buffer pool not found (%lu kB)\n",
             (page_count[i] * pagesize) >> 10);
      return 1;
    }
  }

  /************************************************************
   * Balance
   ************************************************************/
  /* Get active nodes */
  set_configured_nodes(&max_nr_nodes, &node_mask);
  if (max_nr_nodes != 2) {
    printf("Exactly 2 nodes is required\n");
    return 0;
  }

  /* Alloc */
  for (i = 0; i < nr_procs; i++) {
    addr[i] = malloc(sizeof(void *) * page_count[i]);
    status[i] = malloc(sizeof(int *) * page_count[i]);
    nodes[i] = malloc(sizeof(int *) * page_count[i]);
    if (!addr || !status || !nodes) {
      printf("Unable to allocate memory\n");
      return -1;
    }
  }

  /* Set page addresses */
  for (i = 0; i < nr_procs; i++) {
    for (j = 0; j < page_count[i]; j++) {
      addr[i][j] = va_start[i] + j * pagesize;
    }
  }

  /*
   * Read initial map - when @nodes is NULL, @status
   * is filled with the current mapping.
   */
  memset(node_page_count, 0, sizeof(int) * NUMA_MAX_NODES * MAX_PROCS);
  for (i = 0; i < nr_procs; i++) {
    rc = move_pages(pid[i], page_count[i], addr[i], NULL, status[i], 0);
    if (rc < 0) {
      perror("move_pages");
      exit(rc);
    }
    for (j = 0; j < page_count[i]; j++) {
      int n = status[i][j];
      if (n >= 0)
        node_page_count[i][n]++;
    }
    printf("Pid %d current allocation:\n", pid[i]);
    for (j = 0; j < max_nr_nodes; j++) {
      printf("\t%d: %d\n", j, node_page_count[i][j]);
    }
  }

  /*
   * Read free memory counts
   */
  printf("\nNode free counts:\n");
  for (i = 0; i < max_nr_nodes; i++) {
    free_count[i] = get_free_count(i) / pagesize;
    if (free_count[i] < 0) {
      printf("Unable to read free count for node %d\n", i);
      exit(1);
    }
    printf("\t%i: %llu free pages\n", i, free_count[i]);
  }

  /*
   * Choose underallocated node
   */
  target_node = free_count[0] < free_count[1] ? 0 : 1;
  nr_to_swap = abs(free_count[0] - free_count[1]) / 2;

  /* 
   * Re-assign pages
   */
  printf("\nFreeing %d pages on node %d\n", nr_to_swap, target_node);
  memset(valid_page_count, 0, sizeof(unsigned long)*MAX_PROCS);
  for (i = 0; i < nr_procs; i++) {
    for (j = 0; j < page_count[i]; j++) {
      if (!nr_to_swap)
        break;
      if (status[i][j] == target_node) {
        int pn = valid_page_count[i]++;
        addr[i][pn] = addr[i][j];
        nodes[i][pn] = !target_node;
        nr_to_swap--;
      }
    }
  }
  
  /*
   * Swap 
   */
  for (i = 0; i < nr_procs; i++) {
    for (j = 0; j < valid_page_count[i]; j += MAX_MV_PAGES) {
      int page_count = valid_page_count[i];
      int pg_count = page_count - j < MAX_MV_PAGES ? page_count - j : 
        MAX_MV_PAGES;
      rc = move_pages(pid[i], pg_count, &addr[i][j], &nodes[i][j], &status[i][j], 0);
      if (rc < 0 && errno != ENOENT) {
        perror("move_pages");
        exit(rc);
      }
    }
  }
  return 0;
}
