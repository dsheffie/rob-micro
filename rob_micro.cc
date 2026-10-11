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
#include <pthread.h>

#include "perf.hh"

#ifdef __APPLE__
#define MAP_HUGETLB 0
#define MAP_POPULATE 0
#else
#define MAP_JIT 0
#endif

#define PROT (PROT_READ | PROT_WRITE)
#define MAP (MAP_ANONYMOUS|MAP_PRIVATE|MAP_POPULATE)
static const void* failed_mmap = reinterpret_cast<void *>(-1);


//(chain a, chain b, iterations, xor key)
typedef int64_t (*ubench_t)(void*,void*,int64_t,uint64_t);

struct codegen_opts {
  enum class filler {nop, add, jmp, fp, cmp, intfp, all, raw};  
  int unroll;
  int depth;
  bool xor_ptr;
  filler filler_op;
  //-f raw:<hex>[,<hex>...] : arbitrary instruction words, used round robin
  std::vector<uint32_t> raw;
  codegen_opts() :
    unroll(16),
    depth(1),
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

#ifdef __aarch64__
// flip high bits too so stored values don't look like pointers to
// apple's data-memory-dependent prefetcher
static uint64_t xor_key = 0x5a5aa5a55a5aa5a5UL;
#else
// x86 codegen uses a sign-extended imm32 xor
static uint64_t xor_key = 0x13371337UL;
#endif

static uint8_t *rawb = nullptr;
static size_t pgsz = 0;
static list* head = nullptr, *mid = nullptr;
// start each try at random nodes so tries don't rewalk the same lines
static bool random_start = false;
static list *g_nodes = nullptr;
static size_t g_len = 0;
// pick a new xor key (and re-encode the list) before every try
static bool rotate_key = false;

static void rekey_list() {
  uint64_t k = (static_cast<uint64_t>(rand()) << 33) ^
    (static_cast<uint64_t>(rand()) << 11) ^ static_cast<uint64_t>(rand());
  //keep high bits set so stored values never look like pointers
  k |= 1UL << 62;
  uint64_t delta = xor_key ^ k;
  for(size_t i = 0; i < g_len; i++) {
    uint64_t p = reinterpret_cast<uint64_t>(g_nodes[i].next) ^ delta;
    g_nodes[i].next = reinterpret_cast<list*>(p);
  }
  xor_key = k;
}

#ifdef __amd64__
#include "codegen_amd64.hh"
#elif __aarch64__
#include "codegen_aarch64.hh"
#else
#error "wtf is this architecture"
#endif



// clock rate of the last avg_time() run, to spot e-core migration
static double last_ghz = 0.0;
// cpu the last avg_time() run started and ended on (-1 if unknown)
static int last_cpu0 = -1, last_cpu1 = -1;

static int current_cpu() {
#if defined(__APPLE__) && defined(__aarch64__)
  //darwin keeps the cpu number in the low bits of tpidr_el0
  uint64_t r;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(r));
  return r & 0xff;
#else
  return -1;
#endif
}

// time with the wall clock (ns) instead of the cycle counter
static bool wall_clock = false;
// print every try (unix ms, cpus, result), not just the median
static bool verbose = false;

double avg_time(int num_nops, int64_t iterations, codegen_opts &opts) {
  if(rotate_key) {
    rekey_list();
  }
  ubench_t my_bench = make_code(rawb, pgsz, num_nops, opts);
  if(my_bench == nullptr) {
    return 0.0;
  }
  int64_t c = 0;
  if(random_start) {
    head = &g_nodes[rand() % g_len];
    mid = &g_nodes[rand() % g_len];
  }
  if(wall_clock) {
    //no pmu access (e.g. no root): report ns per iteration instead
    last_cpu0 = current_cpu();
    double t0 = timestamp();
    c = my_bench(reinterpret_cast<void*>(head->next),
		 reinterpret_cast<void*>(mid->next),
		 iterations, xor_key);
    double t1 = timestamp();
    last_cpu1 = current_cpu();
    last_ghz = 0.0;
    return (t1-t0)*1e9 / static_cast<double>(c);
  }
  static cycle_counter cc;
  cc.enable_counter();
  cc.reset_counter();

  last_cpu0 = current_cpu();
  double t0 = timestamp();
  uint64_t start = cc.read_counter();
  c = my_bench(reinterpret_cast<void*>(head->next),
	       reinterpret_cast<void*>(mid->next),
	       iterations, xor_key);
  uint64_t stop = cc.read_counter();
  double t1 = timestamp();
  last_cpu1 = current_cpu();
  double links = static_cast<double>(c);
  last_ghz = (stop-start) / ((t1-t0)*1e9);
  
  double avg_cycles = (stop-start)/links;
  return avg_cycles;
}


int main(int argc, char *argv[]) {
  static_assert(sizeof(list)==sizeof(void*), "must be pointer sized");  
  char hostname[256] = {0};
  void *ptr = nullptr;
  bool xor_ptr = false;
  bool use_nops = true;
  std::string filler_name;
  size_t len = 1UL<<22;
  int64_t iters = -1;
  double warmup = 0.0;
  int min_ops = 1;
  int max_ops = 300;
  int step = 1;
  int cpu_lo = -1, cpu_hi = -1;
  int tries = 8;
  int c;
  codegen_opts opts;
  std::vector<double> results;
  srand(time(nullptr));
  
  while ((c = getopt (argc, argv, "b:c:d:ef:i:kl:m:n:q:rs:t:u:vwW:x:")) != -1) {
    switch(c)
      {
      case 'b':
	min_ops = atoi(optarg);
	break;
      case 's':
	step = atoi(optarg);
	break;
      case 'c':
	//only keep tries that start and end on cpus lo:hi
	if(sscanf(optarg, "%d:%d", &cpu_lo, &cpu_hi) != 2) {
	  std::cerr << "-c wants lo:hi\n";
	  return -1;
	}
	break;
      case 'k':
#ifdef __aarch64__
	rotate_key = true;
#else
	std::cerr << "-k needs a 64-bit key (aarch64 only)\n";
	return -1;
#endif
	break;
      case 'r':
	random_start = true;
	break;
      case 'v':
	verbose = true;
	break;
      case 'W':
	warmup = atof(optarg);
	break;
      case 'w':
	wall_clock = true;
	break;
      case 'e':
#ifdef __APPLE__
	thread_qos = QOS_CLASS_BACKGROUND;
#endif
	break;
      case 'q':
#ifdef __APPLE__
	{
	  std::string q(optarg);
	  if(q == "ui") thread_qos = QOS_CLASS_USER_INTERACTIVE;
	  else if(q == "in") thread_qos = QOS_CLASS_USER_INITIATED;
	  else if(q == "ut") thread_qos = QOS_CLASS_UTILITY;
	  else if(q == "bg") thread_qos = QOS_CLASS_BACKGROUND;
	  else {
	    std::cerr << "-q wants ui, in, ut or bg\n";
	    return -1;
	  }
	}
#endif
	break;
      case 'f':
	filler_name = optarg;
	break;
      case 'd':
	opts.depth = atoi(optarg);
	break;
      case 'u':
	opts.unroll = atoi(optarg);
	break;
      case 'i':
	iters = 1L << (atoi(optarg));
	break;
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
#ifdef __APPLE__
  if(wall_clock) {
    //normally done by the pmu setup
    pthread_set_qos_class_self_np(static_cast<qos_class_t>(thread_qos), 0);
  }
#endif
  if(iters < 0) {
    iters = len;
  }
  std::cout << "len = " << len
	    << ", iters = " << iters
	    << ", ops = " << min_ops << ":" << step << ":" << max_ops
	    << ", unroll = " << opts.unroll
	    << ", depth = " << opts.depth
	    << ", tries = " << tries
	    << ", cpus = " << cpu_lo << ":" << cpu_hi
	    << ", timer = " << (wall_clock ? "wall (ns)" : "cycles")
	    << ", warmup = " << warmup
	    << ", random_start = " << random_start
	    << ", rotate_key = " << rotate_key
	    << ", xor_ptr = " << xor_ptr
	    << ", use_nops = " << use_nops    
	    << "\n";
  opts.xor_ptr = xor_ptr;
  if(rotate_key and not(xor_ptr)) {
    std::cerr << "-k needs -x 1\n";
    return -1;
  }
  opts.filler_op = use_nops ? codegen_opts::filler::nop : codegen_opts::filler::add;
  if(filler_name.compare(0, 4, "raw:") == 0) {
    std::string list = filler_name.substr(4);
    size_t pos = 0;
    while(pos < list.size()) {
      size_t e = list.find(',', pos);
      if(e == std::string::npos) e = list.size();
      opts.raw.push_back(static_cast<uint32_t>(strtoul(list.substr(pos, e-pos).c_str(), nullptr, 16)));
      pos = e + 1;
    }
    if(opts.raw.empty()) {
      std::cerr << "raw filler needs at least one instruction word\n";
      return -1;
    }
    opts.filler_op = codegen_opts::filler::raw;
  }
  else if(not(filler_name.empty())) {
    static const std::pair<const char*, codegen_opts::filler> fillers[] = {
      {"nop", codegen_opts::filler::nop},
      {"add", codegen_opts::filler::add},
      {"fp", codegen_opts::filler::fp},
      {"cmp", codegen_opts::filler::cmp},
      {"intfp", codegen_opts::filler::intfp},
      {"all", codegen_opts::filler::all},
      {"jmp", codegen_opts::filler::jmp},
    };
    bool found = false;
    for(const auto &f : fillers) {
      if(filler_name == f.first) {
	opts.filler_op = f.second;
	found = true;
      }
    }
    if(not(found)) {
      std::cerr << "unknown filler " << filler_name
		<< " (nop, add, fp, cmp, intfp, all, jmp)\n";
      return -1;
    }
  }
  std::cout << "filler = "
	    << (filler_name.empty() ? (use_nops ? "nop" : "add") : filler_name)
	    << "\n";
  results.resize(tries);
  

  pgsz = 32*getpagesize();
  rawb = reinterpret_cast<uint8_t*>(mmap(NULL,
					 pgsz,
					 PROT_READ|PROT_WRITE|PROT_EXEC,
					 MAP_ANON|MAP_PRIVATE|MAP_JIT,
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
  g_nodes = nodes;
  g_len = len;
  free(arr);

  if(xor_ptr) {
    for(size_t i = 0; i < len; i++) {
      uint64_t p = reinterpret_cast<uint64_t>(nodes[i].next) ^ xor_key;
      nodes[i].next = reinterpret_cast<list*>(p);
    }
  }

  std::string out_name = std::string(hostname) + std::string(".txt");
  
  std::ofstream out(out_name.c_str());
  
  std::vector<double> ghz(tries);
  if(warmup > 0.0) {
    //run (and discard) the first data point for a while after building
    //the list, so whatever reacts to the setup traffic settles first
    double t_end = timestamp() + warmup;
    int n = 0;
    while(timestamp() < t_end) {
      avg_time(min_ops, iters, opts);
      n++;
    }
    std::cout << "warmup: " << n << " runs over " << warmup << " s\n";
  }
  for(int num_nops=min_ops; num_nops <= max_ops; num_nops += step) {
    for(int t = 0; t < tries; ++t) {
      for(int attempt = 0; ; attempt++) {
	results[t] = avg_time(num_nops, iters, opts);
	ghz[t] = last_ghz;
	bool ok = cpu_lo < 0 or
	  (last_cpu0 == last_cpu1 and last_cpu0 >= cpu_lo and last_cpu0 <= cpu_hi);
	if(verbose) {
	  printf("try %lld %d %d %d %s %.2f\n",
		 static_cast<long long>(timestamp()*1000.0), num_nops,
		 last_cpu0, last_cpu1, ok ? "ok" : "rej", results[t]);
	  fflush(stdout);
	}
	if(ok) {
	  break;
	}
	if(attempt == 100) {
	  std::cerr << "never landed on cpus " << cpu_lo << ":" << cpu_hi << "\n";
	  return -1;
	}
      }
    }
    std::sort(results.begin(), results.end());
    std::sort(ghz.begin(), ghz.end());
    double avg = results[tries/2];
    const char *unit = wall_clock ? " ns" : " cycles";
    std::cout << num_nops << " insns, " << avg << unit;
    if(not(wall_clock)) {
      std::cout << ", " << ghz.front() << "-" << ghz.back() << " GHz";
    }
    std::cout << "\n";
    out << num_nops << " insns, " << avg << unit << "\n";
    out.flush();
  }
  out.close();

  munmap(rawb, pgsz);
  munmap(ptr, sizeof(list)*len);  

  
  return 0;
}
