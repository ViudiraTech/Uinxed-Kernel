#!/bin/bash
# A script to generate a symbol table for a given C source file.
# Usage: nm <FILE> -n | ./kallsyms.sh

sym_count=1

rm -f .symbls.txt .addrs.txt

get_symbol()
{
  echo "$3"
}

get_addr()
{
  echo "$1"
}

while read -r line
do
  # Extract the symbol name and address from the line
  sym_count=$(($sym_count + 1))
  echo -n "\"$(get_symbol $line)\"," >> .symbls.txt
  echo -n "0x$(get_addr $line)," >> .addrs.txt
done

echo "const unsigned long sym_count = $sym_count;" > kallsyms.c
echo "const char *const symbols[$sym_count] = {$(cat .symbls.txt)\"(EOF)\",};" >> kallsyms.c
echo "const unsigned long addresses[$sym_count] = {$(cat .addrs.txt)0xffffffffffffffff,};" >> kallsyms.c
rm -f .symbls.txt .addrs.txt
