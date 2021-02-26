#ifndef _codegen_amd64_
#define _codegen_amd64_

static void save_gprs(int &offs, uint8_t *buf) {
  
  for(int i = 0; i < 8; i++) {
    buf[offs++] = 0x41;
    buf[offs++] = 0x50+i;
  }

}

static void restore_gprs(int &offs, uint8_t *buf) {
  for(int i = 15; i > 7; i--) {
    buf[offs++] = 0x41;
    buf[offs++] = 0x50+i;    
  }
}

static void round_robin_incq(int &offs, uint8_t *buf, int i) {
  buf[offs++] = 0x49;
  buf[offs++] = 0xff;
  buf[offs++] = 0xc0 + (i % 8);
}

ubench_t make_code(uint8_t* buf, size_t buflen, int num_nops, int unroll=4, bool xor_ptr=false) {
  assert(unroll < 128);
  union {
    int32_t i32;
    uint8_t bytes[4];
  } uu;
  //fill with nops
  memset(buf,0x90,buflen);
  //0:	48 89 d0             	mov    %rdx,%rax
  buf[0] = 0xcc;
  //buf[1] = 0x90;
  //buf[2] = 0x90;
  
  int offs = 0;
  save_gprs(offs, buf);
  buf[offs++] = 0x48;
  buf[offs++] = 0x89;
  buf[offs++] = 0xd0;

  const int lloop = offs;
  //0000000000000003 <lloop>:
  for(int iters = 0; iters < unroll; iters++) {
    if(xor_ptr) {
      //48 81 f7 37 13 00 00 	xor    $0x1337,%rdi
      buf[offs++] = 0x48;
      buf[offs++] = 0x81;
      buf[offs++] = 0xf7;
      buf[offs++] = 0x37;
      buf[offs++] = 0x13;
      buf[offs++] = 0x00;
      buf[offs++] = 0x00;      
    }
    
    //3:	48 8b 3f             	mov    (%rdi),%rdi
    buf[offs++] = 0x48;
    buf[offs++] = 0x8b;
    buf[offs++] = 0x3f;
    /* fill with nops */
    for(int i = 0; i < num_nops; i++) {
      round_robin_incq(offs, buf, i);      
      //buf[offs++] = 0x90;
    }
    if(xor_ptr) {
      //48 81 f6 37 13 00 00 	xor    $0x1337,%rsi
      buf[offs++] = 0x48;
      buf[offs++] = 0x81;
      buf[offs++] = 0xf6;
      buf[offs++] = 0x37;
      buf[offs++] = 0x13;
      buf[offs++] = 0x00;
      buf[offs++] = 0x00;      
    }
    
    //146:	48 8b 36             	mov    (%rsi),%rsi
    buf[offs++] = 0x48;
    buf[offs++] = 0x8b;
    buf[offs++] = 0x36;
    /* fill with nops */
    for(int i = 0; i < num_nops; i++) {
      round_robin_incq(offs, buf, i);
      //buf[offs++] = 0x90;
    }
    if(iters == 0) {
      //3 bytes before loop
      int body_sz  = (offs - lloop);
      //10 bytes follow loop
      assert( (lloop + 10 + unroll * body_sz) < pgsz);
    }
  }
  //48 83 ea 01          	sub    $0x1,%rdx
  buf[offs++] = 0x48;
  buf[offs++] = 0x83;
  buf[offs++] = 0xea;
  buf[offs++] = unroll;
  
  //14c:	0f 85 b1 fe ff ff    	jne    3 <lloop>
  const int branch_ip = offs;
  buf[offs++] = 0x0f;
  buf[offs++] = 0x85;
  uu.i32 = lloop - (branch_ip+6);
  for(int i = 0; i < 4; i++) {
    buf[offs++] = uu.bytes[i];
  }
  restore_gprs(offs, buf);
  //152:	c3                   	retq   
  buf[offs++] = 0xc3;
  
  buf[buflen-1] = 0xc3;
  return reinterpret_cast<ubench_t>(buf);
}

#endif
