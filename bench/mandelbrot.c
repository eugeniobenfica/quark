#include <stdio.h>
#include <windows.h>
static double now(void){LARGE_INTEGER f,t;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&t);return (double)t.QuadPart/f.QuadPart;}
static int inside(double cx,double cy){double x=0,y=0;for(int i=0;i<100;i++){double xx=x*x,yy=y*y;if(xx+yy>4.0)return 0;y=2.0*x*y+cy;x=xx-yy+cx;}return 1;}
int main(void){double start=now();int size=1000;long count=0;
  for(long k=0;k<(long)size*size;k++) if(inside((k%size)*3.0/size-2.0,(k/size)*3.0/size-1.5)) count++;
  printf("inside %ld\nms %.1f\n",count,(now()-start)*1000);}
