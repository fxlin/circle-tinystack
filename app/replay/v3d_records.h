#ifndef V3D_RECORDS_H
#define V3D_RECORDS_H

#include "v3d_replay_linux.h"

// ---------------- example recordings ---------------------------- //

#define __maybe_unused

/* load a mem dump, ubench*/
//struct record_entry __maybe_unused v3d_records_loadmem [] = {
//		{type: type_map_gpu_mem,
//		entry_map_gpu_mem :
//			{ start_page: 0x00000020/*page*/,
//				num_pages: 0x00008101/*num_pages*/,
//				is_map: 1/*is_map*/ }
//		},
////		{type_write_gpu_mem_fromfile, .entry_write_gpu_mem_fromfile = { 0x00000000/*page*/, 0x0000ffff/*num_pages*/, "csd_0001"/*tag*/ } },
//		{type : type_eof /* last one */ }
//};

struct record_entry __maybe_unused v3d_records_loadmem [] = {
		{type_map_gpu_mem, .entry_map_gpu_mem = { 0x00000020/*page*/, 0x00008101/*num_pages*/, 1/*is_map*/ } },
		{type_write_gpu_mem_fromfile, .entry_write_gpu_mem_fromfile = { 0x00000000/*page*/, 0x0000ffff/*num_pages*/, "csd_0001"/*tag*/ } },
		{.type = type_eof /* last one */ }
};


#if 0
//#include "gen/records-pyvideocore6.h"
//#include "gen/records-pyvideocore6-2.h"
#include "gen/records-py.h"
#include "gen/records-pysummation.h"
#include "gen/records-headless.h"
#include "gen/records-alexnet.h"
#include "gen/records-vgg16.h"
#include "gen/records-mobilenet.h"
#include "gen/records-resnet18.h"
#include "gen/records-sqz.h"
#include "gen/records-yolov4tiny.h"

static struct v3d_recording * recordings[] =
{
		&recording_py,
		&recording_pysummation,
		&recording_headless,
		&recording_vgg16,
		&recording_alexnet,
		&recording_mobilenet,
		&recording_resnet18,
		&recording_sqz,
		&recording_yolov4tiny,
		NULL /* end */
};
#endif


#endif // V3D_RECORDS_H
