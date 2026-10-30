#include <brdecode.h>
#include <brotli/decode.h>
#include <stdlib.h>
int brotli_buf(const uint8_t *in,size_t n,uint8_t **out,size_t *outlen,size_t max)
{
    *out=NULL;*outlen=0;
    if(!max)max=256u<<20;
    size_t cap=max<16384?max:16384;
    uint8_t *p=malloc(cap?cap:1);if(!p)return -1;
    BrotliDecoderState *s=BrotliDecoderCreateInstance(NULL,NULL,NULL);
    if(!s){free(p);return -1;}
    const uint8_t *next=in;size_t left=n,used=0;
    int result=-1;
    for(;;){
        size_t available=cap-used;uint8_t *dest=p+used;
        BrotliDecoderResult r=BrotliDecoderDecompressStream(s,&left,&next,&available,&dest,NULL);
        used=cap-available;
        if(r==BROTLI_DECODER_RESULT_SUCCESS){result=left? -1:0;break;}
        if(r!=BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT)break;
        if(cap==max){result=1;break;}
        size_t larger=cap>max/2?max:cap*2;
        uint8_t *grown=realloc(p,larger);if(!grown)break;p=grown;cap=larger;
    }
    BrotliDecoderDestroyInstance(s);*out=p;*outlen=used;return result;
}
