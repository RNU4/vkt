#include "render.h"
#include "shader.h"
#include "cmesh.h"
#include "crosshair.h"
#include "mesh.h"
#include "region.h"
#include "config.h"
#include "texture.h"
#include "torch.h"
#include "hud.h"

#include <string>
#include <thread>

GLFWwindow* window;
cmesh_t* cmeshes;

#define MAX_MESHES 20
mesh_t meshes[MAX_MESHES]; int mesh_count = 0;

#define MAX_LIGHTS 10
light_t lights[MAX_LIGHTS]; int light_count = 0;


shader_t  sh_world, sh_cursor, sh_cross, sh_hud;
texture_t tex_atlas;
ubo_t     ubo_view_world, ubo_view_hud, ubo_lights, ubo_fullbright;
view_t    view_world, view_hud;
crosshair_t crosshair;
hud_t hud;

static std::thread thread; static bool thread_active = true;


light_t* render_add_light(vec pos)
{
  if(light_count >= MAX_LIGHTS){
    return NULL;
  }
  lights[light_count].pos = vec_to_vec4(pos);
  light_count++;
  return &lights[light_count - 1];
}

mesh_t* render_addmesh()
{
  if(mesh_count >= MAX_MESHES){
    printf("Over Max Meshes!\n");
    return NULL;
  }  
  mesh_count++;
  return &meshes[mesh_count-1];
}

ubo_t ubo_make(uint size, uint binding) 
{
  ubo_t ubo;
  ubo.size = size;
  glGenBuffers(1, &ubo.handle);
  glBindBuffer(GL_UNIFORM_BUFFER, ubo.handle);
  glBufferData(GL_UNIFORM_BUFFER, ubo.size, (const void*)0, GL_DYNAMIC_DRAW);
  glBindBufferBase(GL_UNIFORM_BUFFER, binding, ubo.handle);
  return ubo;
}

static void draw_mesh(mesh_t* mesh)
{
  if(!(mesh->drawflags & DF_VIS)){
    return;
  } 
  if(mesh->drawflags & DF_DEPTH_TEST){
    glEnable(GL_DEPTH_TEST);
  }
  else{
    glDisable(GL_DEPTH_TEST);
  }
  
  glBindVertexArray(mesh->vao);
  glUseProgram     (mesh->shader);
  glUniformMatrix4fv(0, 1, GL_TRUE, &mesh_makematrix(mesh)[0][0]);
  glUniform4fv(1, 1, &mesh->color[0]);
  glUniform1i (2, mesh->customAttrib);
  glDrawArrays(mesh->primtype, 0, mesh->vertcount);
}

static void draw_cmeshes()
{
  glUseProgram   (sh_world);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture  (GL_TEXTURE_2D, tex_atlas);
  glEnable       (GL_DEPTH_TEST);

  mat4 m_model;
  for(int i = 0; i < CMESH_COUNT; i++)
  {
    // Janky way to limit render dist
    if(glm::length((cmeshes[i].chunk->pos + vec{32.0f,32.0f,32.0f}) - player.pos) > config.renderdist){
      continue;
    }
    
    m_model = glm::translate(mat4(1.0f), cmeshes[i].chunk->pos);
    shader_setMat4(sh_world, "m_model", &m_model[0][0]);
    glBindVertexArray(cmeshes[i].vao);
    glDrawArrays(GL_TRIANGLES, 0, cmeshes[i].vertcount);
  }
}

static void on_resize(GLFWwindow* window, int width, int height) 
{
  glViewport(0, 0, width, height);
}

static void thread_build_cmeshes()
{
  while(!glfwWindowShouldClose(window))
  {
    if(!thread_active){
      continue;
    }
    for(int i = 0; i < CMESH_COUNT; i++)
    {
      if(cmeshes[i].chunk->update == true && !cmeshes[i].needsend){
        cmeshes[i].needsend = true;
        cmesh_build(&cmeshes[i]);
        
        cmeshes[i].chunk->update = false;
        thread_active = false;
        break;
      }
    }
  }
}

static void tick_send_cmeshes()
{
  if(thread_active){
    return;
  }
  for(int i = 0; i < CMESH_COUNT; i++)
  {
    if(cmeshes[i].needsend == true){
      cmesh_send(&cmeshes[i]);
      cmeshes[i].needsend = false;
      thread_active = true;
      return;
    }
  }
}

void render_attach_cmeshes(region_t regions[REGION_COUNT])
{
  int mesh = 0;
  for(int i = 0; i < REGION_COUNT; i++)
  {  
    for(int j = 0; j < REGION_CHUNKS_LENGTH; j++)
    {
      ivec chunkIndex = index3d(j, REGION_CHUNKS_CROOT);
      cmeshes[mesh].chunk = &regions[i].chunks[chunkIndex.x][chunkIndex.y][chunkIndex.z];
      cmeshes[mesh].chunk->update = true;
      mesh++;
    }
  }

  thread = std::thread(thread_build_cmeshes);
}

void render_init()
{
  if(glfwInit() != GLFW_TRUE) { 
    printf("GLFW failed to init\n"); 
    exit(0); 
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 4);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  //glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

  window = glfwCreateWindow(config.width, config.height, "vkt", NULL, NULL);
  
  if(window == NULL) { 
    printf("GLFW failed to create window\n");
    exit(0); 
  }
  glfwMakeContextCurrent(window);

  if(glewInit() != GLEW_OK) { 
    printf("GLEW failed to init\n"); 
    exit(0); 
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glEnable   (GL_DEPTH_TEST);
  // Enable Back Face Culling using Counter Clock Wise order
  glEnable   (GL_CULL_FACE); 
  glFrontFace(GL_CCW );
  glCullFace (GL_BACK);
  // Enable Transparency
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable   (GL_BLEND);
  // Load Shaders
  sh_world  = shader_make("gamedata/shaders/world.vert" , "gamedata/shaders/world.frag" );
  sh_cursor = shader_make("gamedata/shaders/cursor.vert", "gamedata/shaders/cursor.frag");
  sh_cross  = shader_make("gamedata/shaders/cross.vert" , "gamedata/shaders/cross.frag" );
  sh_hud    = shader_make("gamedata/shaders/hud.vert"   , "gamedata/shaders/hud.frag"   );
  // Load Textures
  tex_atlas = texture_make("gamedata/las.jpg");
  // Create Uniform Buffer Objects
  ubo_view_world = ubo_make(128, 0);
  ubo_lights     = ubo_make(32 * MAX_LIGHTS, 1);
  ubo_fullbright = ubo_make(16, 2);
  ubo_view_hud = ubo_make  (128, 3);

  // Crosshair
  glLineWidth(2.0f);
  hud_init(&hud);
  crosshair_init(&crosshair, 0.015f);
  


  glViewport(0,0, config.width, config.height);
  glfwSetFramebufferSizeCallback(window, on_resize);
  
  // Init chunk meshes
  cmeshes = (cmesh_t*)malloc(sizeof(cmesh_t) * CMESH_COUNT);
  for(int i = 0; i < CMESH_COUNT; i++){
    cmesh_init(&cmeshes[i]);
  }
}

void render_tick()
{
  //glViewport(0,0, config.width, config.height);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  tick_send_cmeshes();
  draw_cmeshes();

  for(int i = 0; i < mesh_count; i++){
    draw_mesh(&meshes[i]);  
  }

  view_world.view = glm::lookAt(player.pos, player.pos + player.front, player.up);
  view_world.proj = glm::perspective(glm::radians(float(config.fov)), (float)config.width / (float)config.height, 0.001f, 1000.0f);
  
  // Make HUD projection matrix 
  view_hud.proj = glm::perspective(glm::radians(float(70.0f)), (float)config.width / (float)config.height, 0.001f, 1000.0f);
  
  glBindBuffer(GL_UNIFORM_BUFFER, ubo_view_hud.handle);
  glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)0, sizeof(view_hud), &view_hud);

  glBindBuffer(GL_UNIFORM_BUFFER, ubo_view_world.handle);
  glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)0, sizeof(view_world), &view_world);

  glBindBuffer(GL_UNIFORM_BUFFER, ubo_fullbright.handle);
  glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)0, sizeof(config.fullbright), &config.fullbright);

  glBindBuffer(GL_UNIFORM_BUFFER, ubo_lights.handle);
  glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)0, sizeof(lights), lights);

  glfwSwapBuffers   (window);
  glfwSetWindowTitle(window, ("FPS: " + std::to_string(1.0 / deltaTime)).c_str());
} 

void render_terminate()
{
  thread.join();
  thread.~thread();

  free(cmeshes);
  glfwDestroyWindow(window);
  glfwTerminate(); 
}


