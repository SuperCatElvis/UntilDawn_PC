import struct,sys
def u30(v):
    o=b''
    while True:
        b=v&0x7f; v>>=7
        if v: o+=bytes([b|0x80])
        else: return o+bytes([b])
def s(x): return u30(len(x))+x
names=[b'UDSkipPrompt',b'UDSkipButton',b'UDSkipText']
N=len(names)
strs=[b'',b'Object']+names          # string idx: 1 '', 2 Object, 3.. names
abc=struct.pack('<HH',16,46)+u30(1)*3
abc+=u30(len(strs)+1)+b''.join(s(x) for x in strs)
abc+=u30(2)+bytes([0x16])+u30(1)
abc+=u30(1)
mn=[(1,2)]+[(1,3+i) for i in range(N)]    # mn1 Object, mn2.. classes
abc+=u30(len(mn)+1)+b''.join(bytes([0x07])+u30(a)+u30(b) for a,b in mn)
nm=3*N                                      # per class: script init, iinit, cinit
abc+=u30(nm)+(u30(0)+u30(0)+u30(0)+bytes([0]))*nm
abc+=u30(0)+u30(N)
for i in range(N): abc+=u30(2+i)+u30(1)+bytes([0x01])+u30(0)+u30(3*i+1)+u30(0)
for i in range(N): abc+=u30(3*i+2)+u30(0)
abc+=u30(N)
for i in range(N): abc+=u30(3*i)+u30(1)+u30(2+i)+bytes([0x04])+u30(1)+u30(i)
def body(m,st,loc,isd,msd,c): return u30(m)+u30(st)+u30(loc)+u30(isd)+u30(msd)+u30(len(c))+c+u30(0)+u30(0)
abc+=u30(nm)
for i in range(N):
    c0=bytes([0xd0,0x30,0x65,0x00,0x60,0x01,0x30,0x60,0x01,0x58,i,0x1d,0x68,2+i,0x47])
    abc+=body(3*i,2,1,1,3,c0)+body(3*i+1,1,1,4,5,bytes([0xd0,0x30,0xd0,0x49,0x00,0x47]))+body(3*i+2,1,1,3,4,bytes([0xd0,0x30,0x47]))
def tag(c,b): return struct.pack('<H',c<<6|len(b))+b if len(b)<63 else struct.pack('<HI',c<<6|63,len(b))+b
rect=bytes([0x78,0x00,0x05,0x5f,0x00,0x00,0x0f,0xa0,0x00])
b_=rect+struct.pack('<HH',30<<8,1)+tag(69,struct.pack('<I',8))+tag(82,struct.pack('<I',1)+b'ud\0'+abc)+tag(1,b'')+tag(0,b'')
open(sys.argv[1],'wb').write(b'FWS'+bytes([25])+struct.pack('<I',8+len(b_))+b_)
