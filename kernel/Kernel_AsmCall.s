; 内核调用汇编程序子

global io_hlt

io_hlt: ; void io_hlt(void)
	hlt
	ret
