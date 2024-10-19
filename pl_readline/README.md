# Introduction
一个简单的键盘输入库，计划支持上下左右方向键，tab补全
# Example for Linux
**NOTE**: This example is for Linux only.
```c
#include <stdio.h>
#include <stdlib.h>
#include <termio.h>

int getch(void) {
  struct termios tm, tm_old;
  int fd = 0, ch;

  if (tcgetattr(fd, &tm) < 0) { //保存现在的终端设置
    return -1;
  }

  tm_old = tm;
  cfmakeraw(
      &tm); //更改终端设置为原始模式，该模式下所有的输入数据以字节为单位被处理
  if (tcsetattr(fd, TCSANOW, &tm) < 0) { //设置上更改之后的设置
    return -1;
  }

  ch = getchar();
  if (tcsetattr(fd, TCSANOW, &tm_old) < 0) { //更改设置为最初的样子
    return -1;
  }
  if (ch == 0x0d) {
    return PL_READLINE_KEY_ENTER;
  }
  if (ch == 0x7f) {
    return PL_READLINE_KEY_BACKSPACE;
  }
  if (ch == 0x9) {
    return PL_READLINE_KEY_TAB;
  }
  if (ch == 0x1b) {
    ch = getch();
    if (ch == 0x5b) {
      ch = getch();
      switch (ch) {
      case 0x41:
        return PL_READLINE_KEY_UP;
      case 0x42:
        return PL_READLINE_KEY_DOWN;
      case 0x43:
        return PL_READLINE_KEY_RIGHT;
      case 0x44:
        return PL_READLINE_KEY_LEFT;
      default:
        return -1;
      }
    }
  }
  return ch;
}
void flush() { fflush(stdout); }
void handle_tab(char *buf, pl_readline_words_t words) {
  pl_readline_word_maker_add("hello", words, true, ' ');
  pl_readline_word_maker_add("world", words, false, ' ');
  pl_readline_word_maker_add("foo", words, false, ' ');
  pl_readline_word_maker_add("bar", words, false, ' ');
  pl_readline_word_maker_add("baz", words, false, ' ');
  pl_readline_word_maker_add("qux", words, false, ' ');
}
int main() {
  pl_readline_t n = pl_readline_init(getch, (void *)putchar, flush, handle_tab);
  char *line = malloc(100);
  pl_readline(n,"type something: ", line, 100);
  printf("you typed: %s\n", line);
  free(line);
  pl_readline_uninit(n);
  return 0;
}
```
# feature
- [x] 支持上下左右方向键
- [x] 支持tab补全

# why to write pl_readline?
因为我不想依赖于系统的readline库，而是自己实现一个简单的键盘输入库。在写一个裸机程序时，用这个库可以节省很多时间。当然，你也可以用这个库来为你的操作系统实现shell,因为这个库是以MIT协议发布的。

# how to port to other system?
## basic support
终端需要支持vt100控制字符，能输出字符和读取输入字符，输出字符和输入字符需要没有缓冲，你可以在getch中刷新缓冲。
## extended support
实现Plant OS的vt100扩展功能：`\x1b[C`向右到顶时会自动换行、`\x1b[D`向左到底时会自动换行
这样可以支持多行

# PR

本库支持的功能还不完整，欢迎PR。

# issue

如果有任何bug,请在issue中提出。

**不接受新feature的issue，但接受PR**

但如果是下面的问题，将不会答复：
- linux上无法多行。

发送issue你可能需要知道的：遇到bug请讲明白复现步骤，否则很难帮你解决。

# at last

**HAVE FUN! 祝你玩的开心！**
