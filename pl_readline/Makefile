OBJS_PL_READLINE := plreadln.o plreadln_wordmk.o plreadln_intellisense.o
CC := gcc
CFLAGS := -g -Og -I./include
default: libplreadln.a
	
libplreadln.a: make_build $(OBJS_PL_READLINE)
	cd build && ar rv ../libplreadln.a $(OBJS_PL_READLINE)
	
make_build:
	mkdir -p build
%.o: src/%.c
	$(CC) $(CFLAGS) -c src/$*.c -o build/$*.o
test: libplreadln.a example/name.c
	$(CC) $(CFLAGS) example/name.c -o name.out -L. -lplreadln
