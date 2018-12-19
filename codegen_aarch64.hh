#ifndef _codegen_aarch64_
#define _codegen_aarch64_

ubench_t make_code(uint8_t* buf, size_t buflen, int num_nops, int unroll=4) {
  unroll = 1;
  int offs = 0;
  union u32{
    uint32_t u32;
    int32_t i32;
    uint8_t bytes[4];
  };
  
  union u16{
    int16_t i16;
    uint8_t bytes[2];
  };
  
  memset(buf, 0, buflen);
  
#define WRITE_INSN(X) {				\
    u32 u;					\
    u.u32 = X;					\
    for(int i = 0; i < 4; i++)			\
      buf[offs++] = u.bytes[i];			\
  }
  //aa0203e3 mov x3, x2
  WRITE_INSN(0xaa0203e3);
  
  const int target = offs;
  //f9400000 ldr x0, [x0]
  WRITE_INSN(0xf9400000);

  for(int n = 0; n < num_nops; n++) {
    //nop
    WRITE_INSN(0xd503201f);
  }
  //f9400021 ldr x1, [x1]
  WRITE_INSN(0xf9400021);
    
  for(int n = 0; n < num_nops; n++) {
    //nop
    WRITE_INSN(0xd503201f);
  }
  //f1000463 subs x3, x3, #0x1
  WRITE_INSN(0xf1000463);

  //010x 0100 iiii iiii iiii iiii iiix xxxx  -  b.c ADDR_PCREL19 
  union branch {
    struct br {
      uint32_t op1 : 5;
      int32_t disp : 19;
      uint32_t op0 : 8;
    };
    br bits;
    uint32_t insn;
    branch(int32_t disp) {
      bits.op1 = 0x1;
      bits.op0 = 0x54;
      bits.disp = disp;
    }
  };

  int32_t disp = (target - offs)/4;
  branch b(disp);
  WRITE_INSN(b.insn);
  
  
  //aa0203e0 mov x0, x2  
  WRITE_INSN(0xaa0203e0);
  //4:d65f03c0 ret
  WRITE_INSN(0xd65f03c0);
  
  //24c6ffff 	addiu	a2,a2,-1
  u16 xx;
  buf[offs++] = 0x24;
  buf[offs++] = 0xc6;
  xx.i16 = -unroll;
  buf[offs++] = xx.bytes[0];
  buf[offs++] = xx.bytes[1];
    
  //14c0fffe 	bnez	a2,4 <bar+0x4>
  const int pc4 = offs+4;

  u32 uu;
  buf[offs++] = 0x14;
  buf[offs++] = 0xc0;
  uu.i32 = (target - pc4)/4;
  buf[offs++] = uu.bytes[2];
  buf[offs++] = uu.bytes[3];
  


  //4:d65f03c0 ret
  WRITE_INSN(0xd65f03c0);

  assert(offs < buflen);
  __builtin___clear_cache(buf, buf+buflen);
  return reinterpret_cast<ubench_t>(buf);
}

#undef WRITE_INSN
#endif
