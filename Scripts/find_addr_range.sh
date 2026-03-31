#!/usr/bin/env bash
sym_path="$1"
start_sym="$2"
end_sym="$3"

if [ -z "$sym_path" ] || [ -z "$start_sym" ] || [ -z "$end_sym" ]; then
    echo "Usage: ./find_addr.sh <file_path> <start_symbol> <end_symbol>"
    exit 1
fi
lo=$(awk -v sym="$start_sym" '$0 ~ sym {print $1; exit}' "$sym_path");
hi=$(awk -v sym="$end_sym" '$0 ~ sym {print $1;exit}' "$sym_path");

if [ -z "$lo" ] || [ -z "$hi" ]; then
    echo "Error: Could not find one or both symbols in $sym_path"
    exit 1
fi

echo "range: $lo ~ $hi" > "/dev/stderr" ; 
gawk -v lo="$lo" -v hi="$hi" '
BEGIN{
    num_lo=strtonum("0x" lo);
    num_hi=strtonum("0x" hi);

    printf "Parsed boundaries(hex): 0x%x ~ 0x%x\n", num_lo, num_hi > "/dev/stderr";
}
/^[0-9a-fA-F]{16}/ {
    num_curr=strtonum("0x"$1);
    if(num_curr >=num_lo && num_curr<=num_hi) {print $1, $2}
}'  "$sym_path" | sort -k1,1