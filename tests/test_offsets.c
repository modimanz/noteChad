/* Link each implementation independently. Offsets are legal insertion points. */
#include <windows.h>
#include <stdio.h>
int doc_offset(const WCHAR *text, unsigned long long index);
int main(void){
    const WCHAR s[]={L'a',13,10,0xd83d,0xdc08,L'b',10,0};
    const int expect[]={0,1,3,5,6,7,-1,-1};
    for (int i=0;i<8;++i) if(doc_offset(s,(unsigned)i)!=expect[i]){fprintf(stderr,"offset %d failed\n",i);return 1;}
    if(doc_offset(L"",0)!=0 || doc_offset(L"",1)!=-1 || doc_offset(s,~0ull)!=-1) return 1;
    puts("PASS: CRLF, supplementary characters, EOF, past-end indices");return 0;
}
