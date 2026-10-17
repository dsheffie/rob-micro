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

struct codegen_opts {
  enum class filler {nop, add, jmp};  
  int unroll;
  bool xor_ptr;
  filler filler_op;
  codegen_opts() :
    unroll(16),
    xor_ptr(false),
    filler_op(filler::nop)
  {}
};

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

/* size of the last-level data/unified cache seen by cpu0, from sysfs.
 * returns 0 if sysfs isn't there. sysconf(_SC_LEVEL3_CACHE_SIZE) would
 * be simpler but glibc gets it from cpuid on x86 and usually returns 0
 * on aarch64. */
static size_t llc_bytes() {
  size_t llc_sz = 0;
  int llc_lvl = 0;
  for(int i = 0; ; i++) {
    std::string dir = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(i) + "/";
    std::ifstream lvl_f(dir + "level"), type_f(dir + "type"), size_f(dir + "size");
    if(not lvl_f.good()) {
      break;
    }
    int lvl = 0;
    std::string type;
    size_t sz = 0;
    char unit = 0;
    lvl_f >> lvl;
    type_f >> type;
    size_f >> sz >> unit;
    if(type == "Instruction") {
      continue;
    }
    if(unit == 'K') {
      sz <<= 10;
    }
    else if(unit == 'M') {
      sz <<= 20;
    }
    if(lvl > llc_lvl) {
      llc_lvl = lvl;
      llc_sz = sz;
    }
  }
  return llc_sz;
}

double avg_time(int num_nops, int64_t iterations, codegen_opts &opts) {

  ubench_t my_bench = make_code(rawb, pgsz, num_nops, opts);
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
  bool use_nops = true;
  size_t len = 1UL<<24;
  int max_ops = 300;
  int tries = 8;
  int c;
  codegen_opts opts;
  std::vector<double> results;
  srand(time(nullptr));

  /* the list must blow out the LLC or the loads are cache hits that
   * don't stall long enough to fill the ROB (zen5: 32MB L3 per CCD
   * swallowed the old 1<<22 default). use >= 4x LLC, -l overrides. */
  while(len*sizeof(list) < 4*llc_bytes()) {
    len <<= 1;
  }

  while ((c = getopt (argc, argv, "l:m:n:t:x:")) != -1) {
    switch(c)
      {
      case 'l':
	len = 1UL << (atoi(optarg));
	break;
      case 'n':
	use_nops = (atoi(optarg) != 0);
	break;
      case 'm':
	max_ops = atoi(optarg);
	break;
      case 't':
	tries = atoi(optarg);
	break;
      case 'x':
	xor_ptr = (atoi(optarg) != 0);
	break;
      default:
	break;
      }
  }
  std::cout << "len = " << len
	    << ", max_ops = " << max_ops
	    << ", tries = " << tries
	    << ", xor_ptr = " << xor_ptr
	    << ", use_nops = " << use_nops    
	    << "\n";
  opts.xor_ptr = xor_ptr;
  opts.filler_op = use_nops ? codegen_opts::filler::nop : codegen_opts::filler::add;
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
  
  for(int num_nops=1; num_nops <= max_ops; num_nops++) {
    for(int t = 0; t < tries; ++t) {
       results[t] = avg_time(num_nops,len, opts);
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
