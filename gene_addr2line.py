# to generate addrline_table and print correspnond lineinfo 
# in backtrace to avoid skip out, copy the address and run the addrline later.

import sys
import re
import struct
def main():
    if len(sys.argv)<3:
        print("Usage: python gene_addr2line kernel.asm")
        return
    f_in=open(sys.argv[1], 'r')
    f_out=open(sys.argv[2], 'wb')
    filepattern = re.compile(r'^/(.+):(\d+)$')
    addrpattern = re.compile(r'^\s+([0-9a-fA-F]+):')
    filename_map = {}   # {'proc': 0}
    fileid_list = []
    line_list = []
    addr_list = []
    total_lines = f_in.readlines()
    i = 0
    print("Parsing:")
    while i<len(total_lines):
        match=filepattern.match(total_lines[i])
        if match:
            full_name=match.group(1)
            index=full_name.find("kernel/")
            if index!= -1:
                full_name=full_name[index:]
            if full_name not in filename_map:
                filename_map[full_name] = len(filename_map)
            fileid_list.append(filename_map[full_name])
            line_list.append(int(match.group(2), 10))
            i = i+1
            while i<len(total_lines):
                addr_submatch=addrpattern.match(total_lines[i])
                if addr_submatch:
                    addr_list.append(int(addr_submatch.group(1), 16))
                    break
                i = i+1
        else:
            i = i+1
    # convert into list and sort
    file_map_list=list(filename_map.items())
    file_map_list.sort(key=lambda x :x[1])
    MAGIC = 0x58563641
    name_count=len(file_map_list)
    addr_count=len(addr_list)
    fileid_count=len(fileid_list)
    line_count=len(line_list)
    header=struct.pack('IIIII', MAGIC, name_count, addr_count, fileid_count, line_count)
    f_out.write(header)
    for name, _ in file_map_list:
        bytes_data=name.encode('utf-8') + b'\x00'
        f_out.write(bytes_data)
    for addr in addr_list:  # unsigned int --4bytes(enough for 0x88000000)
        b_data=struct.pack('I', addr)
        f_out.write(b_data)
    for fileid in fileid_list:  # 
        b_data=struct.pack('H', fileid)
        f_out.write(b_data)
    for line in line_list:
        b_data=struct.pack('H', line)
        f_out.write(b_data)
    print("Successfully processing asm file!")
    f_out.close()
    f_in.close()

if __name__=="__main__":
    main()



