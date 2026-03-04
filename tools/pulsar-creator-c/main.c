#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define PATH_SEP '\\'
#else
#include <dirent.h>
#include <unistd.h>
#define PATH_SEP '/'
#endif

#define CONFIG_MAGIC 0x50554C53u
#define INFO_MAGIC 0x494E464Fu
#define CUPS_MAGIC 0x43555053u
#define CONFIG_VERSION 3
#define INFO_VERSION 1
#define CUPS_VERSION 3

static const uint8_t k_course_ids[32] = {
    0x08,0x01,0x02,0x04,0x00,0x05,0x06,0x07,
    0x09,0x0f,0x0b,0x03,0x0e,0x0a,0x0c,0x0d,
    0x10,0x14,0x19,0x1a,0x1b,0x1f,0x17,0x12,
    0x15,0x1e,0x1d,0x11,0x18,0x16,0x13,0x1c
};

static void be16(FILE* f, uint16_t v){ fputc((v>>8)&0xFF,f); fputc(v&0xFF,f);} 
static void be32(FILE* f, uint32_t v){ fputc((v>>24)&0xFF,f); fputc((v>>16)&0xFF,f); fputc((v>>8)&0xFF,f); fputc(v&0xFF,f);} 

static int mkdir_if_missing(const char* p){
#ifdef _WIN32
    if (_mkdir(p)==0 || errno==EEXIST) return 0;
#else
    if (mkdir(p, 0755)==0 || errno==EEXIST) return 0;
#endif
    return -1;
}

static int copy_file(const char* src, const char* dst){
    FILE *in=fopen(src,"rb"), *out=NULL;
    char buf[1<<15]; size_t n;
    if(!in) return -1;
    out=fopen(dst,"wb");
    if(!out){ fclose(in); return -1; }
    while((n=fread(buf,1,sizeof(buf),in))>0){ if(fwrite(buf,1,n,out)!=n){ fclose(in); fclose(out); return -1; }}
    fclose(in); fclose(out); return 0;
}

static int ends_with_szs(const char* s){
    size_t n=strlen(s);
    return n>4 && tolower((unsigned char)s[n-4])=='.' && tolower((unsigned char)s[n-3])=='s' && tolower((unsigned char)s[n-2])=='z' && tolower((unsigned char)s[n-1])=='s';
}

typedef struct { char** v; size_t n, cap; } StrVec;
static void sv_push(StrVec* sv, const char* s){
    if(sv->n==sv->cap){ sv->cap = sv->cap?sv->cap*2:64; sv->v = (char**)realloc(sv->v, sv->cap*sizeof(char*)); }
    sv->v[sv->n++] = strdup(s);
}
static int cmp_str_ci(const void* a,const void* b){
    const char* x=*(const char**)a; const char* y=*(const char**)b;
    for(;*x && *y; x++,y++){ int dx=tolower((unsigned char)*x)-tolower((unsigned char)*y); if(dx) return dx; }
    return (unsigned char)*x - (unsigned char)*y;
}

static int list_szs_files(const char* dir, StrVec* out){
#ifdef _WIN32
    char pat[1024]; snprintf(pat,sizeof(pat),"%s\\*",dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd); if(h==INVALID_HANDLE_VALUE) return -1;
    do{ if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && ends_with_szs(fd.cFileName)) sv_push(out, fd.cFileName);} while(FindNextFileA(h,&fd));
    FindClose(h);
#else
    DIR* d=opendir(dir); struct dirent* e; if(!d) return -1;
    while((e=readdir(d))){
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        if(ends_with_szs(e->d_name)) sv_push(out,e->d_name);
    }
    closedir(d);
#endif
    qsort(out->v, out->n, sizeof(char*), cmp_str_ci);
    return 0;
}

static uint32_t crc32_buf(const unsigned char* data, size_t len){
    uint32_t crc=0xFFFFFFFFu;
    for(size_t i=0;i<len;i++){
        crc ^= data[i];
        for(int j=0;j<8;j++) crc = (crc>>1) ^ (0xEDB88320u & (-(int)(crc & 1u)));
    }
    return ~crc;
}

static uint32_t crc32_file(const char* path){
    FILE* f=fopen(path,"rb"); if(!f) return 0;
    unsigned char buf[1<<15]; size_t n; uint32_t crc=0xFFFFFFFFu;
    while((n=fread(buf,1,sizeof(buf),f))>0){
        for(size_t i=0;i<n;i++){ crc ^= buf[i]; for(int j=0;j<8;j++) crc = (crc>>1) ^ (0xEDB88320u & (-(int)(crc & 1u))); }
    }
    fclose(f); return ~crc;
}

static int read_text(const char* path, char** out){
    FILE* f=fopen(path,"rb"); long sz;
    if(!f) return -1;
    fseek(f,0,SEEK_END); sz=ftell(f); fseek(f,0,SEEK_SET);
    *out=(char*)malloc((size_t)sz+1);
    if(fread(*out,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); return -1; }
    (*out)[sz]=0; fclose(f); return 0;
}

static void replace_token(char** text, const char* token, const char* repl){
    char* src=*text; size_t tl=strlen(token), rl=strlen(repl);
    size_t count=0; for(char* p=src; (p=strstr(p,token)); p+=tl) count++;
    if(!count) return;
    size_t new_len=strlen(src)+count*(rl-tl);
    char* dst=(char*)malloc(new_len+1); char* d=dst; char* p=src; char* hit;
    while((hit=strstr(p,token))){ size_t n=(size_t)(hit-p); memcpy(d,p,n); d+=n; memcpy(d,repl,rl); d+=rl; p=hit+tl; }
    strcpy(d,p); free(*text); *text=dst;
}

int main(int argc, char** argv){
    const char *track_dir=NULL, *mod_name="PulsarPack", *output_dir="output", *date=NULL;
    int wiimmfi_region=0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--track-dir") && i+1<argc) track_dir=argv[++i];
        else if(!strcmp(argv[i],"--mod-name") && i+1<argc) mod_name=argv[++i];
        else if(!strcmp(argv[i],"--output-dir") && i+1<argc) output_dir=argv[++i];
        else if(!strcmp(argv[i],"--wiimmfi-region") && i+1<argc) wiimmfi_region=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--date") && i+1<argc) date=argv[++i];
        else if(!strcmp(argv[i],"--help")){
            printf("Usage: pulsar-creator --track-dir <dir> [--mod-name name] [--output-dir dir] [--wiimmfi-region n] [--date yyyy-mm-dd]\n");
            return 0;
        }
    }
    if(!track_dir){ fprintf(stderr,"--track-dir is required\n"); return 1; }

    char date_buf[32];
    if(!date){ time_t t=time(NULL); struct tm* tm=localtime(&t); strftime(date_buf,sizeof(date_buf),"%Y-%m-%d",tm); date=date_buf; }

    StrVec szs={0};
    if(list_szs_files(track_dir,&szs)!=0 || szs.n==0){ fprintf(stderr,"No .szs files found in %s\n", track_dir); return 1; }
    if(szs.n % 4 != 0){ fprintf(stderr,"Need a multiple of 4 tracks; found %zu\n", szs.n); return 1; }

    char out_base[1024], out_mod[1024], tracks_dir[1024], bins_dir[1024], assets_dir[1024], ghosts_dir[1024], temp_dir[1024], bmg_txt[1024], files_txt[1024], bmg_bmg[1024], config_pul[1024], riivo_dir[1024], riivo_xml[1024];
    snprintf(out_base,sizeof(out_base),"%s",output_dir);
    snprintf(out_mod,sizeof(out_mod),"%s%c%s",output_dir,PATH_SEP,mod_name);
    snprintf(tracks_dir,sizeof(tracks_dir),"%s%cTracks",out_mod,PATH_SEP);
    snprintf(bins_dir,sizeof(bins_dir),"%s%cBinaries",out_mod,PATH_SEP);
    snprintf(assets_dir,sizeof(assets_dir),"%s%cAssets",out_mod,PATH_SEP);
    snprintf(ghosts_dir,sizeof(ghosts_dir),"%s%cGhosts",out_mod,PATH_SEP);
    snprintf(temp_dir,sizeof(temp_dir),"%s%ctemp",out_mod,PATH_SEP);
    snprintf(bmg_txt,sizeof(bmg_txt),"%s%cBMG.txt",temp_dir,PATH_SEP);
    snprintf(files_txt,sizeof(files_txt),"%s%cfiles.txt",temp_dir,PATH_SEP);
    snprintf(bmg_bmg,sizeof(bmg_bmg),"%s%cbmg.bmg",temp_dir,PATH_SEP);
    snprintf(config_pul,sizeof(config_pul),"%s%cConfig.pul",bins_dir,PATH_SEP);
    snprintf(riivo_dir,sizeof(riivo_dir),"%s%cRiivolution",output_dir,PATH_SEP);
    snprintf(riivo_xml,sizeof(riivo_xml),"%s%c%s.xml",riivo_dir,PATH_SEP,mod_name);

    mkdir_if_missing(out_base); mkdir_if_missing(out_mod); mkdir_if_missing(tracks_dir); mkdir_if_missing(bins_dir); mkdir_if_missing(assets_dir); mkdir_if_missing(ghosts_dir); mkdir_if_missing(temp_dir); mkdir_if_missing(riivo_dir);

    char dst[1024];
    snprintf(dst,sizeof(dst),"%s%cLoader.pul",bins_dir,PATH_SEP); if(copy_file("PulsarPackCreator/Resources/Loader.pul", dst)!=0){ fprintf(stderr,"Missing resource Loader.pul\n"); return 1; }
    snprintf(dst,sizeof(dst),"%s%cCode.pul",bins_dir,PATH_SEP); copy_file("PulsarPackCreator/Resources/Code.pul", dst);
    snprintf(dst,sizeof(dst),"%s%cRaceAssets.szs",assets_dir,PATH_SEP); copy_file("PulsarPackCreator/Resources/RaceAssets.szs", dst);
    snprintf(dst,sizeof(dst),"%s%cCommonAssets.szs",assets_dir,PATH_SEP); copy_file("PulsarPackCreator/Resources/CommonAssets.szs", dst);
    snprintf(dst,sizeof(dst),"%s%cUIAssets.szs",assets_dir,PATH_SEP); copy_file("PulsarPackCreator/Resources/UIAssets.szs", dst);

    FILE* ffiles=fopen(files_txt,"w"); FILE* fbmg=fopen(bmg_txt,"w");
    if(!ffiles||!fbmg){ fprintf(stderr,"Failed to create temp files\n"); return 1; }
    fprintf(ffiles,"FILE\n");

    char* bmg_template=NULL;
    if(read_text("PulsarPackCreator/Resources/BMG.txt", &bmg_template)!=0){ fprintf(stderr,"Missing BMG template\n"); return 1; }
    replace_token(&bmg_template,"{CC}","100");
    replace_token(&bmg_template,"{date}",date);
    fputs(bmg_template, fbmg);
    fputs("\n", fbmg);
    free(bmg_template);

    size_t track_count = szs.n;
    size_t cup_count = track_count / 4;
    for(size_t c=0;c<cup_count;c++){
        fprintf(fbmg,"  %X    = Cup %zu\n", (unsigned)(0x10000 + c), c+1);
        fprintf(ffiles,"%zX?\n", c);
        for(size_t t=0;t<4;t++){
            size_t idx=c*4+t;
            char src[1024], name_noext[512];
            strncpy(name_noext, szs.v[idx], sizeof(name_noext)-1); name_noext[sizeof(name_noext)-1]=0;
            char* dot=strrchr(name_noext,'.'); if(dot) *dot='\0';
            snprintf(src,sizeof(src),"%s%c%s",track_dir,PATH_SEP,szs.v[idx]);
            snprintf(dst,sizeof(dst),"%s%c%zu.szs",tracks_dir,PATH_SEP,idx);
            if(copy_file(src,dst)!=0){ fprintf(stderr,"Failed to copy %s\n", src); return 1; }
            fprintf(fbmg,"  %X    = %s\n", (unsigned)(0x20000 + idx), name_noext);
            fprintf(fbmg,"  %X    = Unknown\n", (unsigned)(0x30000 + idx));
            fprintf(ffiles,"%zX=%s||||\n", idx, name_noext);
        }
    }
    fclose(fbmg); fclose(ffiles);

    char cmd[1400];
    snprintf(cmd,sizeof(cmd),"wbmgt encode \"%s\" > /dev/null 2>&1", bmg_txt);
    if(system(cmd)!=0){
#ifdef _WIN32
        snprintf(cmd,sizeof(cmd),"wbmgt encode \"%s\" > NUL 2>&1", bmg_txt);
        if(system(cmd)!=0){
#endif
            fprintf(stderr,"wbmgt failed. Install Wiimm's SZS tools and ensure 'wbmgt' is in PATH.\n");
            return 1;
#ifdef _WIN32
        }
#endif
    }

    // compose config
    FILE* out=fopen(config_pul,"wb"); if(!out){ fprintf(stderr,"Cannot write Config.pul\n"); return 1; }

    uint32_t header_size=36, info_size=80, cups_header_size=28;
    uint32_t fake_extra = (cup_count % 2) ? 4 : 0;
    uint32_t main_track_count = (uint32_t)track_count + fake_extra;
    uint32_t cups_size = cups_header_size + main_track_count*8 + (uint32_t)track_count*2;
    uint32_t offset_info = header_size;
    uint32_t offset_cups = offset_info + info_size;
    uint32_t offset_bmg = offset_cups + cups_size;

    be32(out, CONFIG_MAGIC); be32(out, (uint32_t)CONFIG_VERSION); be32(out, offset_info); be32(out, offset_cups); be32(out, offset_bmg);
    char folder[16]={0}; snprintf(folder,sizeof(folder),"/%s",mod_name); fwrite(folder,1,16,out);

    be32(out, INFO_MAGIC); be32(out, INFO_VERSION); be32(out, info_size);
    srand((unsigned)time(NULL)); be32(out, (uint32_t)rand());
    be32(out, 10); be32(out, 65); be32(out, (uint32_t)wiimmfi_region); be32(out, 0);
    fputc(0,out); fputc(0,out); fputc(0,out); fputc(0,out); fputc(0,out);
    be16(out, (uint16_t)((cup_count<100)?cup_count:100)); fputc(10,out);
    for(int i=0;i<40;i++) fputc(0,out);

    be32(out, CUPS_MAGIC); be32(out, CUPS_VERSION); be32(out, cups_size);
    be16(out, (uint16_t)cup_count); fputc(0,out); fputc(0,out);
    for(int i=0;i<4;i++) be16(out,0);
    be32(out,0);

    for(uint32_t i=0;i<(uint32_t)track_count;i++){
        uint8_t slot=k_course_ids[i%32], music=slot; uint32_t crc;
        char src[1024]; snprintf(src,sizeof(src),"%s%c%s",track_dir,PATH_SEP,szs.v[i]);
        crc=crc32_file(src);
        fputc(slot,out); fputc(music,out); be16(out,0); be32(out,crc);
    }
    if(fake_extra){
        for(int i=0;i<4;i++){
            char src[1024]; snprintf(src,sizeof(src),"%s%c%s",track_dir,PATH_SEP,szs.v[i]);
            fputc(k_course_ids[i%32],out); fputc(k_course_ids[i%32],out); be16(out,0); be32(out,crc32_file(src));
        }
    }

    // alphabetical map
    char** names=(char**)malloc(track_count*sizeof(char*));
    for(size_t i=0;i<track_count;i++){ names[i]=strdup(szs.v[i]); char* d=strrchr(names[i],'.'); if(d)*d=0; }
    char** sorted=(char**)malloc(track_count*sizeof(char*));
    for(size_t i=0;i<track_count;i++) sorted[i]=names[i];
    qsort(sorted, track_count, sizeof(char*), cmp_str_ci);
    for(size_t i=0;i<track_count;i++){
        size_t idx=0; for(;idx<track_count;idx++) if(strcmp(names[idx],sorted[i])==0) break;
        be16(out,(uint16_t)idx);
    }

    FILE* in=fopen(bmg_bmg,"rb"); if(!in){ fprintf(stderr,"Missing generated bmg.bmg\n"); return 1; }
    char io[1<<15]; size_t n;
    while((n=fread(io,1,sizeof(io),in))>0) fwrite(io,1,n,out);
    fclose(in);
    in=fopen(files_txt,"rb"); while((n=fread(io,1,sizeof(io),in))>0) fwrite(io,1,n,out); fclose(in);
    fclose(out);

    char* base_xml=NULL;
    if(read_text("PulsarPackCreator/Resources/Base.xml", &base_xml)==0){
        replace_token(&base_xml, "{$pack}", mod_name);
        FILE* fx=fopen(riivo_xml,"wb"); if(fx){ fputs(base_xml,fx); fclose(fx);} free(base_xml);
    }

    printf("Built Pulsar pack: %s\n", out_mod);
    printf("Tracks: %zu, Cups: %zu\n", track_count, cup_count);

    for(size_t i=0;i<szs.n;i++) free(szs.v[i]);
    free(szs.v); free(names); free(sorted);
    (void)crc32_buf;
    return 0;
}
