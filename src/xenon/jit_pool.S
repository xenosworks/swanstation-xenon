	/* The recompiler's code buffer: 16 MB of near code and 8 MB of far (cpu_code_cache.cpp), in the title's executable
	   .jit pool (docs/GUIDE.md, "Anonymous pages and generated code"). */
	.section .jit,"awx",@nobits
	.balign 0x10000
	.skip 0x1800000
