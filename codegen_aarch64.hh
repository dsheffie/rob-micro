#ifndef _codegen_aarch64_
#define _codegen_aarch64_

ubench_t make_code(uint8_t* buf, size_t buflen, int num_nops, int unroll=4) {
  int offs = 0;
  union {
    int32_t i32;
    uint8_t bytes[4];
  } uu;
  union {
    int16_t i16;
    uint8_t bytes[2];
  } xx;
  memset(buf, 0, buflen);
  //00c01021 	move	v0,a2
  buf[offs++] = 0x00;
  buf[offs++] = 0xc0;
  buf[offs++] = 0x10;
  buf[offs++] = 0x21;
  
  const int target = offs;
  //8c840000 	lw	a0,0(a0)
  buf[offs++] = 0x8c;
  buf[offs++] = 0x84;
  buf[offs++] = 0x00;
  buf[offs++] = 0x00;
  for(int n = 0; n < num_nops; n++) {
    for(int z = 0; z < 4; z++) {
      buf[offs++] = 0x00;
    }
  }
  //  8ca50000 	lw	a1,0(a1)
  buf[offs++] = 0x8c; buf[offs++] = 0xa5;
  buf[offs++] = 0x00; buf[offs++] = 0x00;
  
  for(int n = 0; n < num_nops; n++) {
    for(int z = 0; z < 4; z++) {
      buf[offs++] = 0x00;
    }
  }
  
  //24c6ffff 	addiu	a2,a2,-1
  buf[offs++] = 0x24;
  buf[offs++] = 0xc6;
  xx.i16 = -unroll;
  buf[offs++] = xx.bytes[0];
  buf[offs++] = xx.bytes[1];
    
  //14c0fffe 	bnez	a2,4 <bar+0x4>
  const int pc4 = offs+4;

  buf[offs++] = 0x14;
  buf[offs++] = 0xc0;
  uu.i32 = (target - pc4)/4;
  buf[offs++] = uu.bytes[2];
  buf[offs++] = uu.bytes[3];
  
  //nop
  buf[offs++] = 0x00;
  buf[offs++] = 0x00;
  buf[offs++] = 0x00;
  buf[offs++] = 0x00;
  
  //8:	03e00008 	jr	ra
  buf[offs++] = 0x03;
  buf[offs++] = 0xe3;
  buf[offs++] = 0x00;
  buf[offs++] = 0x08;
  //nop
  buf[offs++] = 0x00;
  buf[offs++] = 0x00;
  buf[offs++] = 0x00;
  buf[offs++] = 0x00;

  assert(offs < buflen);
  
  return reinterpret_cast<ubench_t>(buf);
}

#endif
