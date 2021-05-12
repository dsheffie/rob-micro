#ifndef _codegen_aarch64_
#define _codegen_aarch64_


ubench_t make_code(uint8_t* buf, size_t buflen, int num_nops, codegen_opts &opt) {
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
  struct aarch64_alu_imm_fields {
    uint32_t d : 5;
    uint32_t n : 5;
    uint32_t imm : 12;
    uint32_t SS : 2;
    uint32_t op : 8;
  };
  union aarch64_alu_imm {
    ///x11x 0001 SSii iiii iiii iinn nnnd dddd
    aarch64_alu_imm_fields bits;
    uint32_t insn;
    aarch64_alu_imm(uint32_t i) : insn(i) {}
  };
  
  memset(buf, 0, buflen);
  
  //91026063 add x3, x3, #0x98
  
#define WRITE_INSN(X) {				\
    u32 u;					\
    u.u32 = X;					\
    for(int i = 0; i < 4; i++)			\
      buf[offs++] = u.bytes[i];			\
  }
  //x4 <- 0
  WRITE_INSN(0xd2800004);
  //x5 <- 0
  WRITE_INSN(0xd2800005);
  //x6 <- 0 
  WRITE_INSN(0xd2800006);
  
  aarch64_alu_imm a0(0x91026064);
  aarch64_alu_imm a1(0x91026065);
  aarch64_alu_imm a2(0x91026066);
  a0.bits.imm = 1;
  a0.bits.n = a0.bits.d = 4;
  a1.bits.imm = 1;
  a1.bits.n = a1.bits.d = 5;
  a2.bits.imm = 1;
  a2.bits.n = a2.bits.d = 6;
  
  //aa0203e3 mov x3, x2
  WRITE_INSN(0xaa0203e3);

  if(opt.xor_ptr) {
    //d28266e9 	mov	x9, #0x1337                	// #4919
    //f2a266e9 	movk	x9, #0x1337, lsl #16
    WRITE_INSN(0xd28266e9);
    WRITE_INSN(0xf2a266e9);
  }
  
  const int target = offs;
  for(int iters = 0; iters < opt.unroll; iters++) {
    
    if(opt.xor_ptr) {
      //ca090000 	eor	x0, x0, x9
      WRITE_INSN(0xca090000);
    }
    
    //f9400000 ldr x0, [x0]
    WRITE_INSN(0xf9400000);

    int z = 0;
    for(int n = 0; n < num_nops; n++, z++) {
      //nop
      switch(opt.filler_op)
	{
	case codegen_opts::filler::nop:
	  WRITE_INSN(0xd503201f);
	  break;
	case codegen_opts::filler::add:
	  switch(z % 3)
	    {
	    case 0:
	      WRITE_INSN(a0.insn);
	      break;
	    case 1:
	      WRITE_INSN(a1.insn);
	      break;
	    case 2:
	      WRITE_INSN(a2.insn);
	      break;
	    }
	  break;
	case codegen_opts::filler::jmp:
	default:
	  assert(0);
	}
    }

    if(opt.xor_ptr) {
      //ca050021 	eor	x1, x1, x9
      WRITE_INSN(0xca090021);
    }
    //f9400021 ldr x1, [x1]
    WRITE_INSN(0xf9400021);

    for(int n = 0; n < num_nops; n++, z++) {
      //nop
      switch(opt.filler_op)
	{
	case codegen_opts::filler::nop:
	  WRITE_INSN(0xd503201f);
	  break;
	case codegen_opts::filler::add:
	  switch(z % 3)
	    {
	    case 0:
	      WRITE_INSN(a0.insn);
	      break;
	    case 1:
	      WRITE_INSN(a1.insn);
	      break;
	    case 2:
	      WRITE_INSN(a2.insn);
	      break;
	    }
	  break;
	case codegen_opts::filler::jmp:
	default:
	  assert(0);
	}
    }
    if(iters == 0) {
      //3 bytes before loop
      int body_sz  = (offs - target);
      if( (target + 16 + opt.unroll * body_sz) >= pgsz) {
	return nullptr;
      }
    }
  }

  //f1001063 sub sx3, x3, #0x4
  aarch64_alu_imm s(0xf1001063);
  s.bits.imm = opt.unroll;
  
  WRITE_INSN(s.insn);

  
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
  xx.i16 = -opt.unroll;
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
