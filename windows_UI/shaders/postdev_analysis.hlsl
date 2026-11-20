// One compute device serves the Windows preview, thumbnails and export.
Texture2D<float4> pixels : register(t0);
StructuredBuffer<float4> previous : register(t1);
RWBuffer<uint> bins : register(u0);
RWStructuredBuffer<float4> grid : register(u1);
RWTexture2D<float4> reduced : register(u2);
cbuffer AnalysisSettings : register(b0) {
    int2 size; int2 outputSize;
    int2 origin; int2 offset; // input texture origin, first output cell in this tile
    int2 endCell; int space; int radius;
    int axis; int operation; int stride; int reserved;
};
float linearValue(float x) {
    x=max(0,x);
    if(space==3) return pow(x,1.8);
    if(space==1) return x<1./32 ? x/16 : pow(x,1.8);
    return x<=.04045 ? x/12.92 : pow((x+.055)/1.055,2.4);
}
[numthreads(8,8,1)]
void analyze(uint3 id:SV_DispatchThreadID) {
    int2 c=int2(id.xy)+offset;
    if(any(c>=endCell)) return;
    int2 lo=c*size/outputSize, hi=(c+1)*size/outputSize;
    float sum=0;
    float3 weights=(space==1 || space==3) ? float3(.2880402,.7118741,.0000857) :
        (space==2 ? float3(.2289746,.6917385,.0792869) : float3(.2126,.7152,.0722));
    for(int y=lo.y;y<hi.y;++y) for(int x=lo.x;x<hi.x;++x) {
        float3 rgb=pixels.Load(int3(int2(x,y)-origin,0)).rgb;
        float Y=dot(float3(linearValue(rgb.r),linearValue(rgb.g),linearValue(rgb.b)),weights);
        float l=Y<=216./24389 ? 9.032963*Y : 1.16*pow(Y,1./3)-.16;
        InterlockedAdd(bins[uint(saturate(l)*4095+.5)],1);
        sum+=clamp(log2(max(Y,1e-12)/.184),-20,20);
    }
    float v=sum/((hi.x-lo.x)*(hi.y-lo.y));
    grid[c.y*outputSize.x+c.x]=float4(v,v*v,0,0);
}
[numthreads(1,1,1)]
void percentiles(uint3 id:SV_DispatchThreadID) {
    uint n=uint(size.x)*uint(size.y), trim=(n-1)/1000, count=0, low=0, high=4095;
    for(uint i=0;i<4096;++i) {
        count+=bins[i];
        if(count<=trim) low=i+1;
        if(count>=n-trim) { high=i; break; }
    }
    bins[4096]=asuint(min(1.,(low+.5)/4095.));
    bins[4097]=asuint(max(0.,(high-.5)/4095.));
}
[numthreads(8,8,1)]
void boxFilter(uint3 id:SV_DispatchThreadID) {
    int2 c=id.xy;
    if(any(c>=outputSize)) return;
    int lo=max(0,c[axis]-radius), hi=min(outputSize[axis]-1,c[axis]+radius);
    float2 sum=0;
    [loop] for(int i=lo;i<=hi;++i) {
        int2 q=axis==0 ? int2(i,c.y) : int2(c.x,i);
        sum+=previous[q.y*outputSize.x+q.x].xy;
    }
    float2 v=sum/(hi-lo+1);
    if(operation==1) {
        float var=max(0,v.y-v.x*v.x), a=var/(var+.5);
        v=float2(a,v.x*(1-a));
    }
    grid[c.y*outputSize.x+c.x]=float4(v,asfloat(bins[4096]),asfloat(bins[4097]));
}
[numthreads(8,8,1)]
void downscale(uint3 id:SV_DispatchThreadID) {
    int2 c=int2(id.xy)+offset;
    if(any(c>=endCell)) return;
    int2 lo=c*size, hi=(c+1)*size;
    int2 first=lo/outputSize, end=(hi+outputSize-1)/outputSize;
    float3 sum=0;
    for(int y=first.y;y<end.y;++y) {
        float3 row=0;
        for(int x=first.x;x<end.x;++x) {
            int overlap=min(hi.x,(x+1)*outputSize.x)-max(lo.x,x*outputSize.x);
            row+=pixels.Load(int3(int2(x,y)-origin,0)).rgb*(float(overlap)/size.x);
        }
        int overlap=min(hi.y,(y+1)*outputSize.y)-max(lo.y,y*outputSize.y);
        sum+=row*(float(overlap)/size.y);
    }
    // Typed half-float UAV stores may truncate. Round explicitly to the
    // nearest representable half, matching Qt's RGBX16FPx4 storage.
    float3 step=exp2(max(float3(-24,-24,-24),float3(asuint(abs(sum))>>23)-137));
    reduced[c]=float4(round(sum/step)*step,1);
}
[numthreads(16,16,1)]
void histogram(uint3 id:SV_DispatchThreadID) {
    int2 p=(offset+int2(id.xy))*stride;
    if(any(p>=endCell)) return;
    int2 samplePos=p-origin;
    if(operation==2) samplePos.y/=stride;
    float3 rgb=saturate(pixels.Load(int3(samplePos,0)).rgb);
    uint4 b=uint4(float4(rgb,dot(rgb,float3(.2126,.7152,.0722)))*255+.5);
    InterlockedAdd(bins[b.x],1);
    InterlockedAdd(bins[256+b.y],1);
    InterlockedAdd(bins[512+b.z],1);
    InterlockedAdd(bins[768+b.w],1);
}
[numthreads(256,1,1)]
void normalizeHistogram(uint3 id:SV_GroupThreadID) {
    // One group, shared peak across the four channels, excluding endpoint bins.
    uint k=id.x, n=0;
    for(uint c=0;c<4;++c) n=max(n,bins[c*256+k]);
    if(k>1 && k<254) InterlockedMax(bins[1024],n);
    DeviceMemoryBarrierWithGroupSync();
    for(uint c=0;c<4;++c) bins[c*256+k]=asuint(min(1.,float(bins[c*256+k])/max(1u,bins[1024])));
}
