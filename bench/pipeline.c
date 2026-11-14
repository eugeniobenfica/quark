#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
static double now(void){LARGE_INTEGER f,t;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t);return (double)t.QuadPart/f.QuadPart;}
#else
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
#endif
int main(void){
  int64_t n=200000; int64_t* ks=malloc(sizeof(int64_t)*n); double* xs=malloc(sizeof(double)*n);
  for(int64_t i=0;i<n;i++){ks[i]=i*7919%1000; xs[i]=(i%1000)*0.001;}
  double start=now(); int64_t count=0; double total=0;
  for(int pass=0;pass<300;pass++){
    int64_t least=pass%500, s=0, c=0; double t=0;
    for(int64_t i=0;i<n;i++) if(ks[i]>least) s+=ks[i]*3+1;
    for(int64_t i=0;i<n;i++) if(xs[i]>0.5) c++;
    for(int64_t i=0;i<n;i++) t+=sqrt(xs[i])*0.5;
    count+=s+c; total+=t;
  }
  printf("checksum %lld %.17g\nms %.1f\n",(long long)count,total,(now()-start)*1000);
}
