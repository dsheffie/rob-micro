#include <iostream>
#include <ostream>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "perf.hh"

typedef int64_t (*ubench_t)(void*,void*,int64_t);
struct list {
  list *next = nullptr;
};

inline double timestamp() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + 1e-6*static_cast<double>(tv.tv_usec);
}

template<typename T>
void swap(T &x, T &y) {
  T t = x; x = y; y = t;
}

template <typename T>
void shuffle(T *arr, size_t len) {
  for(size_t i = 0; i < len; i++) {
    size_t j = i + (rand() % (len-i));
    swap(arr[i], arr[j]);
  }
}

static uint8_t *rawb = nullptr;
static size_t pgsz = 0;
static list* head = nullptr, *mid = nullptr;

#ifdef __amd64__
#include "codegen_amd64.hh"
#else
#error "wtf is this architecture"
#endif


double avg_time(int num_nops, int64_t iterations) {

  ubench_t my_bench = make_code(rawb, pgsz, num_nops, 16);
  
  int64_t c = 0;
  cycle_counter cc;
  cc.enable_counter();
  cc.reset_counter();

  uint64_t start = cc.read_counter();
  c = my_bench(reinterpret_cast<void*>(head),
	       reinterpret_cast<void*>(mid),
	       iterations);  
  uint64_t stop = cc.read_counter();
  double links = static_cast<double>(c);
  
  double avg_cycles = (stop-start)/links;

  return avg_cycles;
}


int main(int argc, char *argv[]) {
  char hostname[256] = {0};
  srand(time(nullptr));
  static_assert(sizeof(list)==sizeof(void*), "must be pointer sized");
  pgsz = 4*getpagesize();
  rawb = reinterpret_cast<uint8_t*>(mmap(NULL,
					 pgsz,
					 PROT_READ|PROT_WRITE|PROT_EXEC,
					 MAP_ANON|MAP_PRIVATE,
					 -1,
					 0));
  assert(reinterpret_cast<void*>(-1) != rawb);

  gethostname(hostname,sizeof(hostname));
  
  size_t len = 1UL<<26;
  size_t *arr = nullptr;
  list *nodes = nullptr;
  int rc = posix_memalign((void**)&arr, 64, len*sizeof(size_t));
  assert(rc == 0);
  rc = posix_memalign((void**)&nodes, 64, len*sizeof(list));
  assert(rc == 0);
  for(size_t i = 0; i < len; i++) {
    arr[i] = i;
  }
  shuffle(arr, len);


  for(size_t i = 0; i < (len-1); i++) {
    nodes[arr[i]].next = &nodes[arr[i+1]];
  }

  nodes[arr[len-1]].next = &nodes[arr[0]];
  head = &nodes[arr[0]];
  mid = &nodes[arr[len/2]];
  free(arr);  

  std::string out_name = std::string(hostname) + std::string(".txt");
  
  std::ofstream out(out_name.c_str());
  
  for(int num_nops=1; num_nops < 300; num_nops++) {
    double avg = 0.0, error = 0.0;
    int tries = 1;
    avg = avg_time(num_nops,len);
    do {
      double r = avg_time(num_nops,1L<<16);
      avg = ((avg * tries) +  r) / (tries+1);
      tries++;
      error = std::abs(avg-r) / avg;
      //std::cout << "avg = " << avg << ",r = " << r << ", error = " << error << "\n";
    }
    while(error > 0.01 /*or tries < 16*/);
    std::cout << num_nops << " insns, " << avg << " cycles\n";
    out << num_nops << " insns, " << avg << " cycles\n";
    out.flush();
  }
  out.close();

  munmap(rawb, pgsz);
  free(nodes);


  
  return 0;
}
