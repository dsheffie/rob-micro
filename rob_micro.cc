#include <iostream>
#include <algorithm>
#include <vector>
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

#define PROT (PROT_READ | PROT_WRITE)
#define MAP (MAP_ANONYMOUS|MAP_PRIVATE|MAP_POPULATE)
static const void* failed_mmap = reinterpret_cast<void *>(-1);


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
#elif __aarch64__
#include "codegen_aarch64.hh"
#else
#error "wtf is this architecture"
#endif


double avg_time(int num_nops, int64_t iterations, bool xor_ptr) {

  ubench_t my_bench = make_code(rawb, pgsz, num_nops, 16, xor_ptr);
  if(my_bench == nullptr) {
    return 0.0;
  }
  int64_t c = 0;
  cycle_counter cc;
  cc.enable_counter();
  cc.reset_counter();

  uint64_t start = cc.read_counter();
  c = my_bench(reinterpret_cast<void*>(head->next),
	       reinterpret_cast<void*>(mid->next),
	       iterations);  
  uint64_t stop = cc.read_counter();
  double links = static_cast<double>(c);
  
  double avg_cycles = (stop-start)/links;

  return avg_cycles;
}


int main(int argc, char *argv[]) {
  static_assert(sizeof(list)==sizeof(void*), "must be pointer sized");  
  char hostname[256] = {0};
  void *ptr = nullptr;
  bool xor_ptr = false;
  size_t len = 1UL<<22;
  int max_ops = 300;
  int tries = 8;
  int c;
  std::vector<double> results;
  srand(time(nullptr));
  
  while ((c = getopt (argc, argv, "l:m:t:x:")) != -1) {
    switch(c)
      {
      case 'l':
	len = 1UL << (atoi(optarg));
	break;
      case 'm':
	max_ops = atoi(optarg);
	break;
      case 't':
	tries = atoi(optarg);
	break;
      case 'x':
	xor_ptr = (atoi(optarg) != 0);
      default:
	break;
      }
  }
  std::cout << "len = " << len
	    << ", max_ops = " << max_ops
	    << ", tries = " << tries
	    << ", xor_ptr = " << xor_ptr
	    << "\n";
  results.resize(tries);
  

  pgsz = 32*getpagesize();
  rawb = reinterpret_cast<uint8_t*>(mmap(NULL,
					 pgsz,
					 PROT_READ|PROT_WRITE|PROT_EXEC,
					 MAP_ANON|MAP_PRIVATE,
					 -1,
					 0));
  assert(reinterpret_cast<void*>(-1) != rawb);

  gethostname(hostname,sizeof(hostname));
  

  size_t *arr = nullptr;
  list *nodes = nullptr;

  
  
  int rc = posix_memalign((void**)&arr, 64, len*sizeof(size_t));
  assert(rc == 0);
  ptr = mmap(nullptr, len*sizeof(list), PROT, MAP|MAP_HUGETLB, -1, 0);
  if(ptr == failed_mmap) {
    std::cerr << "unable to use hugepages, trying with "
	      << getpagesize() << " byte pages\n";
    ptr = mmap(nullptr, len*sizeof(list), PROT, MAP, -1, 0);
  }
  assert(ptr != failed_mmap);
  nodes = reinterpret_cast<list*>(ptr);
  
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

  if(xor_ptr) {
    for(size_t i = 0; i < len; i++) {
      uint64_t p = reinterpret_cast<uint64_t>(nodes[i].next) ^ 0x13371337;
      nodes[i].next = reinterpret_cast<list*>(p);
    }
  }

  std::string out_name = std::string(hostname) + std::string(".txt");
  
  std::ofstream out(out_name.c_str());
  
  for(int num_nops=1; num_nops < max_ops; num_nops++) {
    for(int t = 0; t < tries; ++t) {
       results[t] = avg_time(num_nops,len, xor_ptr);
    }
    std::sort(results.begin(), results.end());
    double avg = results[tries/2];
    std::cout << num_nops << " insns, " << avg << " cycles\n";
    out << num_nops << " insns, " << avg << " cycles\n";
    out.flush();
  }
  out.close();

  munmap(rawb, pgsz);
  munmap(ptr, sizeof(list)*len);  

  
  return 0;
}
