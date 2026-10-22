// Throwaway GL probe for Thema 180 step 10: does THIS GL context accept the block formats the
// engine now uploads, as a 2D array, through the per-level "gather all slices, one
// glCompressedTexImage3D" path of uploadTextureArrayGL? Solid-colour slices (a different colour
// each) are encoded with the vendored stb_dxt, uploaded with a full mip chain, sampled back
// through a shader and compared to the colour that went in (sRGB variants sample linear).
// Build: clang++ -std=c++17 -O1 probe.cpp -I<repo>/src/HE_Core/vendor -framework OpenGL -Wno-deprecated-declarations
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#define STB_DXT_IMPLEMENTATION
#define STB_DXT_STATIC
#include <bc7enc/stb_dxt.h>

#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT 0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT 0x8C4C
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif
#define GL_RED_RGTC1 0x8DBB
#define GL_RG_RGTC2 0x8DBD

enum Fmt { BC1, BC3, BC4, BC5 };
static const char* nm[] = {"BC1","BC3","BC4","BC5"};
static int bb(Fmt f){ return (f==BC1||f==BC4)?8:16; }
static size_t lvlBytes(Fmt f,int w,int h){ return size_t((w+3)/4)*((h+3)/4)*bb(f); }

static std::vector<uint8_t> encode(Fmt f,int w,int h,const uint8_t rgb[3]){
  int bw=(w+3)/4,bh=(h+3)/4; std::vector<uint8_t> out(size_t(bw)*bh*bb(f));
  uint8_t blk[64]; for(int i=0;i<16;i++){blk[i*4]=rgb[0];blk[i*4+1]=rgb[1];blk[i*4+2]=rgb[2];blk[i*4+3]=200;}
  uint8_t r[16],rg[32]; for(int i=0;i<16;i++){r[i]=rgb[0];rg[i*2]=rgb[0];rg[i*2+1]=rgb[1];}
  for(int b=0;b<bw*bh;b++){ uint8_t* d=&out[size_t(b)*bb(f)];
    if(f==BC1) stb_compress_dxt_block(d,blk,0,2); else if(f==BC3) stb_compress_dxt_block(d,blk,1,2);
    else if(f==BC4) stb_compress_bc4_block(d,r); else stb_compress_bc5_block(d,rg); }
  return out;
}
static GLuint sh(GLenum t,const char*s){GLuint h=glCreateShader(t);glShaderSource(h,1,&s,0);glCompileShader(h);GLint ok;glGetShaderiv(h,GL_COMPILE_STATUS,&ok);if(!ok){char l[2048];glGetShaderInfoLog(h,2048,0,l);fprintf(stderr,"compile: %s\n",l);exit(2);}return h;}
static float s2l(float c){return c<=0.04045f?c/12.92f:powf((c+0.055f)/1.055f,2.4f);}

int main(){
  CGLPixelFormatAttribute at[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAAccelerated,(CGLPixelFormatAttribute)0};
  CGLPixelFormatObj pf;GLint np;CGLChoosePixelFormat(at,&pf,&np);CGLContextObj cx;CGLCreateContext(pf,0,&cx);CGLSetCurrentContext(cx);
  printf("GL %s | %s\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
  GLuint fbo,rt;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glGenTextures(1,&rt);glBindTexture(GL_TEXTURE_2D,rt);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,1,1,0,GL_RGBA,GL_FLOAT,0);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
  GLuint vs=sh(GL_VERTEX_SHADER,"#version 410 core\nvoid main(){vec2 p=vec2(float((gl_VertexID&1)<<2)-1.0,float((gl_VertexID&2)<<1)-1.0);gl_Position=vec4(p,0.0,1.0);}");
  GLuint fs=sh(GL_FRAGMENT_SHADER,"#version 410 core\nuniform sampler2DArray t;uniform float layer;uniform float lod;out vec4 o;void main(){o=textureLod(t,vec3(0.5,0.5,layer),lod);}");
  GLuint pr=glCreateProgram();glAttachShader(pr,vs);glAttachShader(pr,fs);glLinkProgram(pr);glUseProgram(pr);
  GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);glViewport(0,0,1,1);
  const int W=16,H=16,mips=5,layers=3; int fail=0;
  const uint8_t cols[3][3]={{200,40,40},{40,200,40},{40,40,200}};
  for(int f=BC1;f<=BC5;f++) for(int srgb=0;srgb<2;srgb++){
    if(f>=BC4&&srgb) continue;
    GLenum fmt = f==BC1?(srgb?GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:GL_COMPRESSED_RGB_S3TC_DXT1_EXT)
               : f==BC3?(srgb?GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:GL_COMPRESSED_RGBA_S3TC_DXT5_EXT)
               : f==BC4?GL_RED_RGTC1:GL_RG_RGTC2;
    // asset layout: slice-major, each slice its chain
    std::vector<std::vector<uint8_t>> sl(layers);
    for(int s=0;s<layers;s++){ int lw=W,lh=H; for(int l=0;l<mips;l++){ auto e=encode((Fmt)f,lw,lh,cols[s]); sl[s].insert(sl[s].end(),e.begin(),e.end()); lw=std::max(1,lw>>1);lh=std::max(1,lh>>1);} }
    GLuint id;glGenTextures(1,&id);glBindTexture(GL_TEXTURE_2D_ARRAY,id);
    int lw=W,lh=H; size_t off=0; while(glGetError());
    for(int l=0;l<mips;l++){ size_t lb=lvlBytes((Fmt)f,lw,lh); std::vector<uint8_t> g(lb*layers);
      for(int s=0;s<layers;s++) memcpy(&g[lb*s],&sl[s][off],lb);
      glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY,l,fmt,lw,lh,layers,0,(GLsizei)g.size(),g.data());
      off+=lb; lw=std::max(1,lw>>1);lh=std::max(1,lh>>1);}
    GLenum err=glGetError();
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAX_LEVEL,mips-1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    printf("%s%s upload glError=0x%x\n",nm[f],srgb?" sRGB":"",err);
    if(err){fail++;continue;}
    glUniform1i(glGetUniformLocation(pr,"t"),0);
    double worst=0;
    for(int s=0;s<layers;s++) for(int l=0;l<mips;l++){
      glUniform1f(glGetUniformLocation(pr,"layer"),(float)s);glUniform1f(glGetUniformLocation(pr,"lod"),(float)l);
      glDrawArrays(GL_TRIANGLES,0,3); float px[4]; glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,px);
      float want[3]; for(int c=0;c<3;c++){ float v=cols[s][c]/255.f; want[c]= srgb?s2l(v):v; }
      if(f==BC4){want[1]=0;want[2]=0;} if(f==BC5){want[2]=0;}
      double e=0; for(int c=0;c<3;c++) e=std::max(e,(double)fabsf(px[c]-want[c]));
      worst=std::max(worst,e);
      if(e>0.03){printf("  MISMATCH slice %d level %d got %.3f %.3f %.3f want %.3f %.3f %.3f\n",s,l,px[0],px[1],px[2],want[0],want[1],want[2]);fail++;}
    }
    printf("  worst abs error over %d slices x %d levels: %.4f\n",layers,mips,worst);
  }
  printf(fail?"PROBE FAILED (%d)\n":"PROBE OK\n",fail);
  return fail?1:0;
}
