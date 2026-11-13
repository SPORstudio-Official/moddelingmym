#include "da.h"
#include <stdio.h>
static FB fb;
static void bmp(const char *p) {
    FILE *f = fopen(p, "wb"); if (!f) return;
    int rowsize = fb.w*3, pad = (4-(rowsize%4))%4, datasize = (rowsize+pad)*fb.h;
    unsigned char hdr[54] = {0}; hdr[0]='B'; hdr[1]='M';
    int fsize = 54+datasize, off = 54, ih = 40;
    memcpy(hdr+2,&fsize,4); memcpy(hdr+10,&off,4); memcpy(hdr+14,&ih,4);
    memcpy(hdr+18,&fb.w,4); memcpy(hdr+22,&fb.h,4);
    short pl=1,bpp=24; memcpy(hdr+26,&pl,2); memcpy(hdr+28,&bpp,2); memcpy(hdr+34,&datasize,4);
    fwrite(hdr,1,54,f);
    unsigned char *row = calloc(1,rowsize+pad);
    for (int y=fb.h-1;y>=0;y--){for(int x=0;x<fb.w;x++){u32 c=fb.px[y*fb.w+x];row[x*3]=c&0xff;row[x*3+1]=(c>>8)&0xff;row[x*3+2]=(c>>16)&0xff;}
        fwrite(row,1,rowsize+pad,f);}
    free(row); fclose(f); printf("wrote %s\n", p);
}
int main(void) {
    fb.w = 640; fb.h = 360; fb.px = calloc(fb.w*fb.h, sizeof(u32));
    float scales[3] = {1.5f, 2.6f, 3.8f};
    fb_clear(&fb, rgb_(14, 10, 10));
    for (int i=0;i<3;i++) draw_entity_face(&fb, 110+i*210, 185, scales[i], 0.9f + i, 255);
    bmp("/tmp/faces.bmp");
    return 0;
}
