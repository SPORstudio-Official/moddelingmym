import sys
from PIL import Image
def conv(src,dst):
    f=open(src,'rb').read(); i=0; fields=[]
    while len(fields)<4:
        while f[i:i+1].isspace(): i+=1
        if f[i:i+1]==b'#':
            while f[i:i+1] not in (b'\n',b''): i+=1
            continue
        j=i
        while not f[j:j+1].isspace(): j+=1
        fields.append(f[i:j]); i=j
    i+=1
    w,h=int(fields[1]),int(fields[2])
    Image.frombytes('RGB',(w,h),f[i:i+w*h*3]).save(dst)
for a in sys.argv[1:]:
    conv(a, a.rsplit('.',1)[0]+'.png')
