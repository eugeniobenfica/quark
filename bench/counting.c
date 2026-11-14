#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static double now(void){LARGE_INTEGER f,t;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t);return (double)t.QuadPart/f.QuadPart;}
#else
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
#endif
static int64_t fmod64(int64_t a,int64_t b){int64_t r=a%b;if(r&&((r^b)<0))r+=b;return r;}
static int64_t fdiv64(int64_t a,int64_t b){int64_t q=a/b;if((a%b)&&((a^b)<0))q--;return q;}
static int64_t blend(int64_t a,int64_t b,int64_t c){return (int64_t)floor(0.2126*a+0.7152*b+0.0722*c+0.5);}
int main(void){
  int64_t n=384*216; int64_t* rows=malloc(sizeof(int64_t)*n*3);
  for(int64_t i=0;i<n*3;i++) rows[i]=fmod64(fdiv64((int64_t)((uint64_t)i*2654435761u+12345u),7),256);
  double start=now(); int64_t checksum=0;
  for(int pass=0;pass<200;pass++){
    int64_t first[256]={0},second[256]={0},third[256]={0},blended[256]={0};
    for(int64_t p=0;p<n;p++){first[rows[p*3]]++;}
    for(int64_t p=0;p<n;p++){second[rows[p*3+1]]++;}
    for(int64_t p=0;p<n;p++){third[rows[p*3+2]]++;}
    for(int64_t p=0;p<n;p++){blended[blend(rows[p*3],rows[p*3+1],rows[p*3+2])]++;}
    checksum+=first[pass%256]+second[7]+third[200]+blended[pass%256];
  }
  printf("checksum %lld\nms %.1f\n",(long long)checksum,(now()-start)*1000);
}
