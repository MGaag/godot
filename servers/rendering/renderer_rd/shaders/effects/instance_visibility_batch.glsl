#[compute]
#version 450

#VERSION_DEFINES

// Constant descriptor bindings preserve Vulkan portability without descriptor indexing.
layout(local_size_x = 64) in;
#define DECLARE_COMMAND(d) layout(set=0,binding=1+BATCH_SIZE+d,std430) readonly buffer SourceCommand##d { uint words[]; } src_command##d;
JOB_SWITCH(DECLARE_COMMAND)
#undef DECLARE_COMMAND
#define DECLARE_OUTPUT_COMMAND(d) layout(set=0,binding=1+3*BATCH_SIZE+d,std430) buffer VisibleCommand##d { uint words[]; } dst_command##d;
JOB_SWITCH(DECLARE_OUTPUT_COMMAND)
#undef DECLARE_OUTPUT_COMMAND
struct JobParams {
 vec4 planes[16];
 vec4 origin_tile_height;
 float radius;
 uint max_instances;
 uint source_width;
 uint layout_info;
};
layout(set=0,binding=1+4*BATCH_SIZE,std430) readonly buffer Parameters { JobParams jobs[]; } parameters;
layout(push_constant,std430) uniform BatchParams { uint count; } batch;
#ifdef MODE_PREPARE
layout(set=0,binding=2+4*BATCH_SIZE,std430) buffer Control { uint words[]; } control;
#else
layout(set=0,binding=2+4*BATCH_SIZE,std430) readonly buffer Control { uint words[]; } control;
layout(set=0,binding=0) uniform sampler2D source_positions[BATCH_SIZE];
#define DECLARE_SOURCE(d) layout(set=0,binding=1+d,std430) readonly buffer SourceInstances##d { float words[]; } src##d;
JOB_SWITCH(DECLARE_SOURCE)
#undef DECLARE_SOURCE
#define DECLARE_IDS(d) layout(set=0,binding=1+2*BATCH_SIZE+d,std430) buffer VisibleInstances##d { uint words[]; } dst##d;
JOB_SWITCH(DECLARE_IDS)
#undef DECLARE_IDS
#endif
uint command_word(uint job,uint word) {
#define READ_COMMAND(d) case uint(d): return src_command##d.words[word];
 switch(job) { JOB_SWITCH(READ_COMMAND) }
#undef READ_COMMAND
 return 0u;
}
uint source_count(uint job) { return min(command_word(job,1u),parameters.jobs[job].max_instances); }
uint mode_index(uint job) { return 4u+BATCH_SIZE+job; }
uint leader_index(uint job) { return 4u+2u*BATCH_SIZE+job; }
#ifdef MODE_PREPARE
void zero_draw(uint job) {
#define ZERO_DRAW(d) case uint(d): dst_command##d.words[1]=0u; return;
 switch(job) { JOB_SWITCH(ZERO_DRAW) }
#undef ZERO_DRAW
}
float ordered_to_float(uint value) {
 return uintBitsToFloat((value&0x80000000u)!=0u ? value^0x80000000u : ~value);
}
// Bounds describe exact selected roots, while each draw retains its own animated sphere.
uint classify_roots(uint job) {
 if((command_word(job,19u)&1u)==0u) return 0u;
 JobParams p=parameters.jobs[job];
 vec3 low=vec3(ordered_to_float(command_word(job,8u)),ordered_to_float(command_word(job,10u)),ordered_to_float(command_word(job,9u)));
 vec3 high=vec3(ordered_to_float(command_word(job,11u)),ordered_to_float(command_word(job,13u)),ordered_to_float(command_word(job,12u)));
 low=low*vec3(p.origin_tile_height.z,p.origin_tile_height.w,p.origin_tile_height.z)+vec3(p.origin_tile_height.x,0,p.origin_tile_height.y);
 high=high*vec3(p.origin_tile_height.z,p.origin_tile_height.w,p.origin_tile_height.z)+vec3(p.origin_tile_height.x,0,p.origin_tile_height.y);
 vec3 center=(low+high)*0.5, extent=(high-low)*0.5;
 float tolerance=0.0011+0.000001*max(max(max(abs(low.x),abs(high.x)),max(abs(low.y),abs(high.y))),max(abs(low.z),abs(high.z)));
 bool inside=true;
 for(uint plane=0u;plane<(p.layout_info&255u);++plane) {
  float distance=dot(p.planes[plane].xyz,center)+p.planes[plane].w;
  float reach=dot(abs(p.planes[plane].xyz),extent);
  if(distance+reach<-(p.radius+tolerance)) return 2u;
  if(distance-reach< -p.radius+0.001) inside=false;
 }
 return inside ? 1u : 0u;
}
// Only a producer-issued exact predicate/pose identity permits reuse. Equal
// counts or bounds alone do not establish that two streams contain the same roots.
bool same_selection(uint a,uint b) {
 if((command_word(a,19u)&2u)==0u || (command_word(b,19u)&2u)==0u) return false;
 if(source_count(a)!=source_count(b)) return false;
 if(parameters.jobs[a].max_instances!=parameters.jobs[b].max_instances || parameters.jobs[a].source_width!=parameters.jobs[b].source_width || parameters.jobs[a].layout_info!=parameters.jobs[b].layout_info) return false;
 if(any(notEqual(parameters.jobs[a].origin_tile_height,parameters.jobs[b].origin_tile_height))) return false;
 for(uint word=14u;word<19u;++word) if(command_word(a,word)!=command_word(b,word)) return false;
 return true;
}
void main() {
 if(gl_LocalInvocationIndex!=0u) return;
 for(uint job=0u;job<batch.count;++job) {
  uint mode=source_count(job)==0u ? 2u : classify_roots(job);
  control.words[mode_index(job)]=mode;
  control.words[leader_index(job)]=job;
  for(uint previous=0u;previous<job;++previous) {
   if(same_selection(job,previous)) { control.words[leader_index(job)]=control.words[leader_index(previous)]; break; }
  }
  if(mode==2u) zero_draw(job);
 }
 uint groups=0u;
 control.words[3]=0u;
 for(uint job=0u;job<batch.count;++job) {
  bool needed=false;
  if(control.words[leader_index(job)]==job) {
   for(uint component=job;component<batch.count;++component) {
    needed=needed || (control.words[leader_index(component)]==job && control.words[mode_index(component)]!=2u);
   }
  }
  if(needed) groups+=(source_count(job)+63u)/64u;
  control.words[4u+job]=groups;
 }
 control.words[0]=groups;control.words[1]=1u;control.words[2]=1u;
}
#else
float source_word(uint job,uint word) {
#define READ_SOURCE(d) case uint(d): return src##d.words[word];
 switch(job) { JOB_SWITCH(READ_SOURCE) }
#undef READ_SOURCE
 return 0.0;
}
vec4 position_at(uint job,ivec2 pixel) {
#define READ_POSITION(d) case uint(d): return texelFetch(source_positions[d],pixel,0);
 switch(job) { JOB_SWITCH(READ_POSITION) }
#undef READ_POSITION
 return vec4(0);
}
uint reserve_block(uint job, uint count) {
#define JOB_RESERVE_BLOCK(descriptor) case uint(descriptor): return atomicAdd(dst##descriptor.words[parameters.jobs[job].max_instances], count);
 switch (job) { JOB_SWITCH(JOB_RESERVE_BLOCK) }
#undef JOB_RESERVE_BLOCK
 return 0u;
}

uint complete_group(uint job) {
#define JOB_COMPLETE_GROUP(descriptor) case uint(descriptor): return atomicAdd(dst##descriptor.words[parameters.jobs[job].max_instances + 1u], 1u) + 1u;
 switch (job) { JOB_SWITCH(JOB_COMPLETE_GROUP) }
#undef JOB_COMPLETE_GROUP
 return 0u;
}

void publish_count(uint job) {
#define JOB_PUBLISH_COUNT(descriptor) case uint(descriptor): dst_command##descriptor.words[1] = atomicExchange(dst##descriptor.words[parameters.jobs[job].max_instances], 0u); atomicExchange(dst##descriptor.words[parameters.jobs[job].max_instances + 1u], 0u); return;
 switch (job) { JOB_SWITCH(JOB_PUBLISH_COUNT) }
#undef JOB_PUBLISH_COUNT
}

void write_cell(uint job, uint index, uint cell) {
#define JOB_WRITE_CELL(descriptor) case uint(descriptor): dst##descriptor.words[index] = cell; return;
 switch (job) { JOB_SWITCH(JOB_WRITE_CELL) }
#undef JOB_WRITE_CELL
}


// A matching wood/foliage selection reuses this fetch even if the original
// compactors assigned different array orders. Stable cell identity is preserved.
bool load_root(uint job,uint instance,out uint cell,out vec3 center) {
 JobParams p=parameters.jobs[job];
 if(instance>=source_count(job)) return false;
 uint stride=(p.layout_info>>8u)&255u, index_word=(p.layout_info>>16u)&255u, bank=(p.layout_info>>24u)&1u;
 cell=uint(source_word(job,(instance+bank*p.max_instances)*stride+index_word)+0.5);
 vec4 position=position_at(job,ivec2(int(cell%p.source_width),int(cell/p.source_width)));
 center=vec3(p.origin_tile_height.x+position.r*p.origin_tile_height.z,position.b*p.origin_tile_height.w,p.origin_tile_height.y+position.g*p.origin_tile_height.z);
 return true;
}
bool accepts_root(uint job,vec3 center) {
 JobParams p=parameters.jobs[job];
 float tolerance=0.001+0.000001*max(max(abs(center.x),abs(center.y)),abs(center.z));
 for(uint plane=0u;plane<(p.layout_info&255u);++plane) {
  if(dot(p.planes[plane].xyz,center)+p.planes[plane].w<-(p.radius+tolerance)) return false;
 }
 return true;
}
shared uint accepted_count;
shared uint output_base;
void main() {
 uint leader=0u;
 while(leader+1u<batch.count && gl_WorkGroupID.x>=control.words[4u+leader]) ++leader;
 uint instance=(gl_WorkGroupID.x-control.words[3u+leader])*64u+gl_LocalInvocationIndex;
 uint cell=0u;vec3 center=vec3(0);
 bool valid=load_root(leader,instance,cell,center);
 uint group_count=control.words[4u+leader]-control.words[3u+leader];
 for(uint job=leader;job<batch.count;++job) {
  if(control.words[leader_index(job)]!=leader || control.words[mode_index(job)]==2u) continue;
  if(gl_LocalInvocationIndex==0u) accepted_count=0u;
  barrier();
  bool accepted=valid && (control.words[mode_index(job)]==1u || accepts_root(job,center));
  uint slot=0u;
  if(accepted) slot=atomicAdd(accepted_count,1u);
  barrier();
  if(gl_LocalInvocationIndex==0u && accepted_count>0u) output_base=reserve_block(job,accepted_count);
  barrier();
  if(accepted) write_cell(job,output_base+slot,cell);
  memoryBarrierBuffer();
  barrier();
  if(gl_LocalInvocationIndex==0u && complete_group(job)==group_count) publish_count(job);
 }
}
#endif
