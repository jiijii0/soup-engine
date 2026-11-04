#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
static void ref(int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7,int*o){
 int p=5793*(a0+a4),q=5793*(a0-a4);int r0=7568*a2+3135*a6,r1=3135*a2-7568*a6;
 int e0=p+r0,e1=q+r1,e2=q-r1,e3=p-r0;
 int o0=8035*a1+6811*a3+4551*a5+1598*a7,o1=6811*a1-1598*a3-8035*a5-4551*a7,o2=4551*a1-8035*a3+1598*a5+6811*a7,o3=1598*a1-4551*a3+6811*a5-8035*a7;
 o[0]=e0+o0;o[7]=e0-o0;o[1]=e1+o1;o[6]=e1-o1;o[2]=e2+o2;o[5]=e2-o2;o[3]=e3+o3;o[4]=e3-o3;}
static void fast(int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7,int*o){
 int p=5793*(a0+a4),q=5793*(a0-a4);int z=3135*(a2+a6);int r0=z+4433*a2,r1=z-10703*a6;
 int e0=p+r0,e1=q+r1,e2=q-r1,e3=p-r0;
 int z5=6811*(a1+a3+a5+a7);int z1=-5213*(a7+a1),z2=-14846*(a5+a3),z3=-11362*(a7+a3)+z5,z4=-2260*(a5+a1)+z5;
 int o3=1729*a7+z1+z3,o2=11893*a5+z2+z4,o1=17799*a3+z2+z3,o0=8697*a1+z1+z4;
 o[0]=e0+o0;o[7]=e0-o0;o[1]=e1+o1;o[6]=e1-o1;o[2]=e2+o2;o[5]=e2-o2;o[3]=e3+o3;o[4]=e3-o3;}
int main(){uint64_t s=88172645463325252ull;long bad=0,n=0;
 for(long it=0;it<20000000;it++){int a[8];for(int k=0;k<8;k++){s^=s<<13;s^=s>>7;s^=s<<17;int lim=(it&3)==0?65536:(it&3)==1?4096:(it&3)==2?256:16;a[k]=(int)(s%(2*lim+1))-lim;}
  int x[8],y[8];ref(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],x);fast(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],y);
  for(int k=0;k<8;k++)if(x[k]!=y[k]){bad++;break;}n++;}
 printf("%ld blocks, %ld differ\n",n,bad);return bad!=0;}
