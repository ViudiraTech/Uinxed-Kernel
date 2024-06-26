/* 内核程序入口 */

#include "Kernel_Head.h"

void kernel_entry()
{
	for(;;){
		io_hlt();
	}
}
