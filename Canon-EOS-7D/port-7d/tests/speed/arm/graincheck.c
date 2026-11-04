#include <stdio.h>
int main(){int out=0;
 for(int grain=0;grain<=100;grain++){int amp=(grain*38)/100;
  for(int n=0;n<65536;n++){int d=((n-32768)*amp)>>15;
   for(int v=0;v<256;v++){int g=(v*(255-v))>>6;int r=v+((d*g)>>8);if(r<0||r>255)out++;}}}
 printf("out-of-range results: %d\n",out);return out!=0;}
