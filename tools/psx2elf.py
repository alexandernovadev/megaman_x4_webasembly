import sys,struct
d=open(sys.argv[1],'rb').read()
pc,gp,taddr,tsize=struct.unpack('<4I',d[16:32])
code=d[0x800:0x800+tsize]
# 0x80010000.. code; plus BSS up to 2MB RAM as zero-fill
memsz=0x80200000-taddr
eh=52; ph=32; off=0x1000
hdr=b'\x7fELF'+bytes([1,1,1,0])+b'\0'*8
hdr+=struct.pack("<HHIIIIIHHHHHH",2,8,1,pc,eh,0,0x50000000,eh,ph,1,0,0,0)  # ET_EXEC, EM_MIPS, EF_MIPS_ARCH_32 (REA legacy lane)
phdr=struct.pack('<8I',1,off,taddr,taddr,len(code),memsz,7,0x1000)
o=hdr+phdr; o+=b'\0'*(off-len(o))+code
open(sys.argv[2],'wb').write(o)
print("entry %08x load %08x size %x memsz %x"%(pc,taddr,len(code),memsz))
