// Affine Granlund-Montgomery test for r | (Q*d + 1), vs. group-reduce baseline.
// gcc -O2 -fno-tree-vectorize -march=native gm_affine.c -o gm
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>
typedef unsigned __int128 u128;

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static inline uint64_t rnd(void){ rng_s ^= rng_s<<7; rng_s ^= rng_s>>9; return rng_s; }

static inline uint64_t inv64(uint64_t r){ uint64_t x=r; for(int i=0;i<5;i++) x*=2-r*x; return x; }

// exact remainder of 3-limb number (little endian) by r
static uint64_t mod3(const uint64_t a[3], uint64_t r){
  u128 rem=0; for(int i=2;i>=0;i--){ rem=((rem<<64)|a[i])%r; } return (uint64_t)rem;
}

#define MAXR 8000
static uint64_t R[MAXR], RINV[MAXR], LIM[MAXR], W[MAXR];
static int NR;

// baseline groups: products of consecutive primes < 2^63, Montgomery constants
#define MAXG 4000
static uint64_t GM[MAXG], GNINV[MAXG], GR1[MAXG], GC1[MAXG], GC2[MAXG]; static int GLO[MAXG], GHI[MAXG], NG;

static inline uint64_t redc(u128 T, uint64_t M, uint64_t ninv){ // T < M*2^64 -> T*2^-64 mod M
  uint64_t m=(uint64_t)T*ninv; u128 t=(T+(u128)m*M)>>64; uint64_t r=(uint64_t)t; return r>=M? r-M : r;
}
static uint64_t powmod(uint64_t b,uint64_t e,uint64_t m){u128 r=1,x=b%m;while(e){if(e&1)r=r*x%m;x=x*x%m;e>>=1;}return (uint64_t)r;}

int main(int argc,char**argv){
  int maxp = argc>1? atoi(argv[1]) : 65536;
  int D = 1<<15;
  // sieve odd primes < maxp
  char *c=calloc(maxp,1); NR=0;
  for(int i=3;i<maxp && NR<MAXR;i+=2) if(!c[i]){ R[NR++]=i; for(long j=(long)i*i;j<maxp;j+=2*i) c[j]=1; }
  for(int i=0;i<NR;i++){ RINV[i]=inv64(R[i]); LIM[i]=UINT64_MAX/R[i]; }
  // groups
  NG=0; for(int i=0;i<NR;){ u128 M=1; int lo=i; while(i<NR && M*R[i] < ((u128)1<<63)) M*=R[i++];
    uint64_t m=(uint64_t)M; GM[NG]=m; GNINV[NG]=-inv64(m);
    uint64_t R1=(uint64_t)(((u128)1<<64)%m);           // 2^64 mod M
    GR1[NG]=R1;
    GC1[NG]=(uint64_t)((u128)R1*R1%m);                  // 2^128 mod M
    GC2[NG]=(uint64_t)((u128)GC1[NG]*R1%m);             // 2^192 mod M
    GLO[NG]=lo; GHI[NG]=i; NG++; }

  // node: random 160-bit Q (odd), D random d < 2^32
  uint64_t Q[3]={rnd(),rnd(),rnd()&0xFFFFFFFFull}; Q[0]|=1;
  uint64_t *d=malloc(D*8),*buf1=malloc(D*8),*buf2=malloc(D*8);
  for(int j=0;j<D;j++) d[j]=rnd()>>32;
  // plant some hits: d = -Q^{-1} mod r for random r
  for(int j=0;j<D;j+=97){ int i=rnd()%NR; uint64_t r=R[i],qr=mod3(Q,r); if(!qr) continue;
    uint64_t t=(r-powmod(qr,r-2,r))%r; d[j]=t+r*((rnd()>>40)%( (1ull<<32)/r )); }

  // ---- per-node precompute for the affine test ----
  for(int i=0;i<NR;i++){ uint64_t qr=mod3(Q,R[i]); W[i]=qr*RINV[i]; }

  // ---- correctness: affine test vs exact remainder of p=Q*d+1 ----
  long mism=0, hits=0;
  for(int j=0;j<4096;j++){ uint64_t P[3]; u128 t=(u128)Q[0]*d[j]+1; P[0]=(uint64_t)t;
    t=(t>>64)+(u128)Q[1]*d[j]; P[1]=(uint64_t)t; t=(t>>64)+(u128)Q[2]*d[j]; P[2]=(uint64_t)t;
    for(int i=0;i<NR;i++){ int a = d[j]*W[i]+RINV[i] <= LIM[i]; int b = mod3(P,R[i])==0; hits+=b; mism+=(a!=b); } }
  printf("primes: %d (3..%d), groups: %d, correctness: %ld mismatches over %ld tests (%ld true hits)\n",
         NR,(int)R[NR-1],NG,mism,4096L*NR,hits);

  int REP=20; uint64_t t0,t1; long surv=0;
  // ---- A: affine test, branchy early exit ----
  t0=__rdtsc();
  for(int rep=0;rep<REP;rep++){ surv=0;
    for(int j=0;j<D;j++){ uint64_t x=d[j]; int bad=0;
      for(int i=0;i<NR;i++) if(x*W[i]+RINV[i]<=LIM[i]){bad=1;break;}
      surv+=!bad; } }
  t1=__rdtsc(); double cA=(double)(t1-t0)/REP/D; long survA=surv;

  // ---- B: affine test, staged branchless with survivor compaction ----
  t0=__rdtsc();
  for(int rep=0;rep<REP;rep++){ uint64_t *in=d,*out=buf1; size_t n=D;
    for(int lo=0;lo<NR && n;){ int blk = lo<64? 8 : 32; int hi=lo+blk; if(hi>NR) hi=NR; size_t m=0;
      for(size_t j=0;j<n;j++){ uint64_t x=in[j]; uint64_t bad=0;
        for(int i=lo;i<hi;i++) bad |= (x*W[i]+RINV[i]<=LIM[i]);
        out[m]=x; m+=!bad; }
      n=m; in=out; out=(out==buf1)?buf2:buf1; lo=hi; }
    surv=n; }
  t1=__rdtsc(); double cB=(double)(t1-t0)/REP/D; long survB=surv;

  // ---- C: baseline: p=Q*d+1 (192-bit), Montgomery-reduce mod group product, G-M per prime ----
  t0=__rdtsc();
  for(int rep=0;rep<REP;rep++){ surv=0;
    for(int j=0;j<D;j++){ uint64_t x=d[j],P0,P1,P2; u128 t=(u128)Q[0]*x+1; P0=(uint64_t)t;
      t=(t>>64)+(u128)Q[1]*x; P1=(uint64_t)t; t=(t>>64)+(u128)Q[2]*x; P2=(uint64_t)t;
      int bad=0;
      for(int g=0;g<NG && !bad;g++){ uint64_t M=GM[g],ni=GNINV[g];
        // p mod M = redc(P0*2^128) + redc(P1*2^128)*... : use redc(Pk * (2^(64k+64) mod M))
        uint64_t s0=redc((u128)P0*GR1[g],M,ni);   // P0        mod M
        uint64_t s1=redc((u128)P1*GC1[g],M,ni);   // P1*2^64   mod M
        uint64_t s2=redc((u128)P2*GC2[g],M,ni);   // P2*2^128  mod M
        uint64_t s=s0; s+=s1; if(s>=M) s-=M; s+=s2; if(s>=M) s-=M;  // = p mod M
        for(int i=GLO[g];i<GHI[g];i++) if(s*RINV[i]<=LIM[i]){bad=1;break;}
      }
      surv+=!bad; } }
  t1=__rdtsc(); double cC=(double)(t1-t0)/REP/D; long survC=surv;

  // ---- per-node update cost: Q_r <- Q_r*q mod r, W = Q_r*rinv (Barrett) ----
  static uint64_t MU[MAXR], QR[MAXR]; for(int i=0;i<NR;i++){ MU[i]=UINT64_MAX/R[i]; QR[i]=mod3(Q,R[i]); }
  int NODES=2000; uint64_t sink=0;
  t0=__rdtsc();
  for(int k=0;k<NODES;k++){ uint64_t q=(rnd()>>32)|1;
    for(int i=0;i<NR;i++){ uint64_t r=R[i],mu=MU[i];
      uint64_t qm=q-(uint64_t)(((u128)q*mu)>>64)*r; if(qm>=r) qm-=r;
      uint64_t a=QR[i]*qm; uint64_t v=a-(uint64_t)(((u128)a*mu)>>64)*r; if(v>=r) v-=r;
      QR[i]=v; W[i]=v*RINV[i]; } sink+=W[k%NR]; }
  t1=__rdtsc(); double cN=(double)(t1-t0)/NODES/NR;

  printf("survivors: A=%ld B=%ld C=%ld (of %d)\n",survA,survB,survC,D);
  printf("ref cycles per d (all primes, early exit):\n  A affine, branchy        %8.1f\n  B affine, staged/compact %8.1f\n  C 192-bit group reduce   %8.1f\n",cA,cB,cC);
  printf("ref cycles per (node, prime) to update Q mod r and W: %.2f  (sink %llu)\n",cN,(unsigned long long)sink);
  return 0;
}
