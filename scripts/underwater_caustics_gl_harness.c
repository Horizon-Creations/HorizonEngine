// Offscreen OpenGL 4.1 harness for the underwater caustic fragment shader (Thema 185 Schritt 3).
// Renders the shader of OpenGLRenderer.cpp (kUnderwaterCausticsFS, @TRAINS@ filled in by hand)
// over a flat floor with the depth reconstruction swapped for an analytic ray/plane hit, and
// writes a PPM. It exists because the editor build is slow: it shows the SHAPE of the net
// (docs/images/underwater-caustics-gl-harness-*.png), not the engine integration.
//
// Build: clang -O1 -o harness underwater_caustics_gl_harness.c -framework OpenGL -Wno-deprecated-declarations
// Run:   ./harness frag.glsl out.ppm INTENSITY CLOCK_SECONDS CAM_Y FLOOR_Y   (surface plane is y = 0)
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static char* slurp(const char* p){FILE*f=fopen(p,"rb");if(!f){perror(p);exit(1);}fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);char*b=malloc(n+1);fread(b,1,n,f);b[n]=0;fclose(f);return b;}
static GLuint sh(GLenum t,const char*s){GLuint h=glCreateShader(t);glShaderSource(h,1,&s,0);glCompileShader(h);GLint ok;glGetShaderiv(h,GL_COMPILE_STATUS,&ok);if(!ok){char l[4096];glGetShaderInfoLog(h,4096,0,l);fprintf(stderr,"compile: %s\n",l);exit(2);}return h;}
int main(int argc,char**argv){
  const char* fsPath=argv[1]; const char* out=argv[2]; float intensity=atof(argv[3]); float tsec=atof(argv[4]); float camY=atof(argv[5]); float floorY=atof(argv[6]);
  CGLPixelFormatAttribute at[]={kCGLPFAOpenGLProfile,(CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core,kCGLPFAAccelerated,(CGLPixelFormatAttribute)0};
  CGLPixelFormatObj pf;GLint np;CGLChoosePixelFormat(at,&pf,&np);CGLContextObj cx;CGLCreateContext(pf,0,&cx);CGLSetCurrentContext(cx);
  fprintf(stderr,"GL %s\n",glGetString(GL_VERSION));
  const int W=960,H=540;
  GLuint fbo,tex;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,W,H,0,GL_RGBA,GL_FLOAT,0);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
  float px[4]={0.10f,0.13f,0.12f,1};GLuint scn;glGenTextures(1,&scn);glBindTexture(GL_TEXTURE_2D,scn);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,1,1,0,GL_RGBA,GL_FLOAT,px);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  GLuint vs=sh(GL_VERTEX_SHADER,"#version 410 core\nout vec2 vUV;void main(){vec2 p=vec2(float((gl_VertexID&1)<<2)-1.0,float((gl_VertexID&2)<<1)-1.0);vUV=p*0.5+0.5;gl_Position=vec4(p,0.0,1.0);}");
  GLuint fs=sh(GL_FRAGMENT_SHADER,slurp(fsPath));GLuint pr=glCreateProgram();glAttachShader(pr,vs);glAttachShader(pr,fs);glLinkProgram(pr);
  GLint ok;glGetProgramiv(pr,GL_LINK_STATUS,&ok);if(!ok){char l[4096];glGetProgramInfoLog(pr,4096,0,l);fprintf(stderr,"link: %s\n",l);return 3;}
  GLuint vao;glGenVertexArrays(1,&vao);glBindVertexArray(vao);glViewport(0,0,W,H);glUseProgram(pr);
  glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,scn);glUniform1i(glGetUniformLocation(pr,"uScene"),0);
  glUniform4f(glGetUniformLocation(pr,"uCamSurface"),0,camY,0,0.0f); /* surface Y = 0 */
  float sx=0.3f,sy=0.9f,sz=0.2f;float sl=sqrtf(sx*sx+sy*sy+sz*sz);glUniform4f(glGetUniformLocation(pr,"uSun"),sx/sl,sy/sl,sz/sl,0);
  glUniform4f(glGetUniformLocation(pr,"uSunColor"),3.0f,2.8f,2.4f,0);
  glUniform4f(glGetUniformLocation(pr,"uCaustics"),intensity,1.0f,tsec,14.0f);
  glUniform1f(glGetUniformLocation(pr,"uFloorY"),floorY);
  glDrawArrays(GL_TRIANGLES,0,3);
  float* buf=malloc(W*H*4*sizeof(float));glReadPixels(0,0,W,H,GL_RGBA,GL_FLOAT,buf);
  FILE*f=fopen(out,"wb");fprintf(f,"P6\n%d %d\n255\n",W,H);
  for(int y=H-1;y>=0;--y)for(int x=0;x<W;++x){for(int c=0;c<3;++c){float v=buf[(y*W+x)*4+c];v=v/(1.0f+v);v=powf(v,1.0f/2.2f);int i=(int)(v*255.0f+0.5f);if(i>255)i=255;if(i<0)i=0;fputc(i,f);}}
  fclose(f);return 0;}
