#include <iostream>
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

ubench_t make_code(uint8_t* buf, size_t buflen, int num_nops) {
  union {
    int32_t i32;
    uint8_t bytes[4];
  } uu;
  //fill with nops
  memset(buf,0x90,buflen);
  //0:	48 89 d0             	mov    %rdx,%rax
  //buf[0] = 0xcc;
  //buf[1] = 0x90;
  //buf[2] = 0x90;

  buf[0] = 0x48;
  buf[1] = 0x89;
  buf[2] = 0xd0;
  
  //0000000000000003 <lloop>:
  //3:	48 8b 3f             	mov    (%rdi),%rdi
  buf[3] = 0x48;
  buf[4] = 0x8b;
  buf[5] = 0x3f;
  /* fill with nops */
  for(int i = 0; i < num_nops; i++) {
    buf[6+i] = 0x90;
  }
  //146:	48 8b 36             	mov    (%rsi),%rsi
  buf[num_nops+6] = 0x48;
  buf[num_nops+7] = 0x8b;
  buf[num_nops+8] = 0x36;
  // 48 ff ca             	dec    %rdx
  buf[num_nops+9] = 0x48;
  buf[num_nops+10] = 0xff;
  buf[num_nops+11] = 0xca;
  //14c:	0f 85 b1 fe ff ff    	jne    3 <lloop>
  buf[num_nops+12] = 0x0f;
  buf[num_nops+13] = 0x85;
  uu.i32 = 3 - (num_nops+18);
  for(int i = 0; i < 4; i++) {
    buf[num_nops+14+i] = uu.bytes[i];
  }
  //152:	c3                   	retq   
  buf[num_nops+18] = 0xc3;

  buf[buflen-1] = 0xc3;
  return reinterpret_cast<ubench_t>(buf);
}



double avg_time(int num_nops, int64_t iterations) {

  ubench_t my_bench = make_code(rawb, pgsz, num_nops);
  
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


int main() {
  srand(time(nullptr));
  static_assert(sizeof(list)==sizeof(void*), "must be 8bytes");
  pgsz = getpagesize();
  rawb = reinterpret_cast<uint8_t*>(mmap(NULL,
					 pgsz,
					 PROT_READ|PROT_WRITE|PROT_EXEC,
					 MAP_ANON|MAP_PRIVATE,
					 -1,
					 0));
  assert(reinterpret_cast<void*>(-1) != rawb);

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
  
  for(int num_nops=1; num_nops < 300; num_nops++) {
    double avg = 0.0, error = 0.0;
    int tries = 1;
    avg = avg_time(num_nops,len);
    do {
      double r = avg_time(num_nops,1L<<23);
      avg = ((avg * tries) +  r) / (tries+1);
      tries++;
      error = std::abs(avg-r) / avg;
      //std::cout << "avg = " << avg << ",r = " << r << ", error = " << error << "\n";
    }
    while(error > 0.01 and tries < 16);
    std::cout << num_nops << " insns, " << avg << " cycles\n";
  }

  munmap(rawb, pgsz);
  free(nodes);


  
  return 0;
}
