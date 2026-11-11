#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
static double now(void){LARGE_INTEGER f,t;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t);return (double)t.QuadPart/f.QuadPart;}
static int64_t fmod64(int64_t a,int64_t b){int64_t r=a%b;if(r&&((r^b)<0))r+=b;return r;}
static int64_t fdiv64(int64_t a,int64_t b){int64_t q=a/b;if((a%b)&&((a^b)<0))q--;return q;}
static int64_t luma(int64_t r,int64_t g,int64_t b){return (int64_t)floor(0.2126*r+0.7152*g+0.0722*b+0.5);}
int main(void){
  int64_t n=384*216; int64_t* rgb=malloc(sizeof(int64_t)*n*3);
  for(int64_t i=0;i<n*3;i++) rgb[i]=fmod64(fdiv64((int64_t)((uint64_t)i*2654435761u+12345u),7),256);
  double start=now(); int64_t checksum=0;
  for(int frame=0;frame<200;frame++){
    int64_t red[256]={0},green[256]={0},blue[256]={0},light[256]={0};
    for(int64_t p=0;p<n;p++){red[rgb[p*3]]++;}
    for(int64_t p=0;p<n;p++){green[rgb[p*3+1]]++;}
    for(int64_t p=0;p<n;p++){blue[rgb[p*3+2]]++;}
    for(int64_t p=0;p<n;p++){light[luma(rgb[p*3],rgb[p*3+1],rgb[p*3+2])]++;}
    checksum+=red[frame%256]+green[7]+blue[200]+light[frame%256];
  }
  printf("checksum %lld\nms %.1f\n",(long long)checksum,(now()-start)*1000);
}
