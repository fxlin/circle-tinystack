// porting of v3d_replay.c from the linux kernel
// spin this off from v3d.cpp beacuse of many C syntax unsupported by g++

#include "v3d_records.h"

// xzl: must sync with mesa v3dv_bo.c
enum v3d_create_bo_type_xzl {
	V3D_CREATE_BO_DEVICE	 = 1,	//	"device_alloc"
	V3D_CREATE_BO_CL,				//	"CL"
	V3D_CREATE_BO_TSDA,			//	"TSDA"
	V3D_CREATE_BO_TILE,			// 	"tile_alloc"
	V3D_CREATE_BO_SHARED,		// 	"descriptor pool bo"
	V3D_CREATE_BO_VKCMD,		// "vkCmdUpdateBuffer"
	V3D_CREATE_BO_SPILL,		// "spill"
	V3D_CREATE_BO_VI, 			// "default_vi_attributes"
	V3D_CREATE_BO_QUERY, 		// "query"
	V3D_CREATE_BO_CONST,			// "push constants"
	// coord_shader_assembly, vertex_shader_assembly, fragment_shader_assembly,compute_shader_assembly
	V3D_CREATE_BO_SHADER,
	V3D_CREATE_BO_UNKNOWN, // EOF
};

const char *v3d_create_bo_names[] = {
		[V3D_CREATE_BO_DEVICE] = "device_alloc",
		[V3D_CREATE_BO_CL] = "CL",
		[V3D_CREATE_BO_TSDA] = "TSDA",
		[V3D_CREATE_BO_TILE] = "tile_alloc",
		[V3D_CREATE_BO_SHARED] = "descriptor pool bo",
		[V3D_CREATE_BO_VKCMD] = "vkCmdUpdateBuffer",
		[V3D_CREATE_BO_SPILL] = "spill",
		[V3D_CREATE_BO_VI] = "default_vi_attributes",
		[V3D_CREATE_BO_QUERY] = "query",
		[V3D_CREATE_BO_CONST] = "push constants",
		[V3D_CREATE_BO_SHADER] = "shader_assembly",
};

// what bos to dump?
const int v3d_dump_bo_type[] = {
		[V3D_CREATE_BO_DEVICE] = 	0,
		[V3D_CREATE_BO_CL] = 			1,
		[V3D_CREATE_BO_TSDA] = 		1,		// needed by rcl
		[V3D_CREATE_BO_TILE] = 		0,
		[V3D_CREATE_BO_SHARED] = 	1,
		[V3D_CREATE_BO_VKCMD] = 	1,
		[V3D_CREATE_BO_SPILL] = 	1,
		[V3D_CREATE_BO_VI] = 			1,
		[V3D_CREATE_BO_QUERY] = 	1,
		[V3D_CREATE_BO_CONST] = 	1,
		[V3D_CREATE_BO_SHADER] = 	1,
		[V3D_CREATE_BO_UNKNOWN] = 1,
};


#include "v3d_records.h"
