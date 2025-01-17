/*
 * Copyright 2019 Xilinx Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#include <sys/time.h>

#include <iostream>
#include <vector>
#include <numeric>
#include <chrono>

// host_src includes
#include "xclhal2.h"
#include "xclbin.h"

// lowlevel common include
#include "xrt_utils.h"
#include "xf_pppre_hw_64.h"      

class PPHandle {
public:

	xclDeviceHandle handle;
	uint64_t cu_base_addr;
	unsigned cu_index;
	signed char *imageFromDevice_ptr;
	unsigned paramsbuf_bo;
	uint64_t bo_paramsbuf_devAddr;
	unsigned img_inp_bo;
	unsigned char *img_inptr;
	uint64_t bo_input_phy_addr;
	float *paramsbuf_ptr;
	unsigned execHandle;
	void *execData;
};

extern PPHandle* pphandle;

int preprocess(PPHandle * &pphandle,
		unsigned char* inimg_data,
		int img_ht,
		int img_wt,
		int org_ht,
		int org_wt)
{
	xclDeviceHandle handle = pphandle->handle;
	uint64_t cu_base_addr = pphandle->cu_base_addr;
	unsigned paramsbuf_bo = pphandle-> paramsbuf_bo;
	uint64_t bo_paramsbuf_devAddr = pphandle-> bo_paramsbuf_devAddr;
	float *paramsbuf_ptr = pphandle-> paramsbuf_ptr;
	unsigned img_inp_bo = pphandle->img_inp_bo;
	unsigned char* img_inptr = pphandle->img_inptr;
	uint64_t bo_input_phy_addr = pphandle->bo_input_phy_addr;	

	unsigned execHandle = pphandle->execHandle;
	void *execData =  pphandle->execData;

	int th1 = 127, th2 = 128;

    float scale_height = (float)org_ht/(float)img_ht;
    float scale_width = (float)org_wt/(float)img_wt;
	int out_height_resize, out_width_resize;

    if(scale_width<scale_height) {
    	out_width_resize = org_wt;
    	out_height_resize = (int)((float)(img_ht*org_wt)/(float)img_wt);
    }
    else
    {
    	out_width_resize = (int)((float)(img_wt*org_ht)/(float)img_ht);
    	out_height_resize = org_ht;
    }  
		
	const int imageToDevice_size = img_wt*img_ht*3*sizeof(char);
	std::memcpy(img_inptr, inimg_data, imageToDevice_size);

	// Send the input imageToDevice data to the device memory
	if(xclSyncBO(handle, img_inp_bo, XCL_BO_SYNC_BO_TO_DEVICE , imageToDevice_size, 0)) 
		return 1;

	try {
		auto ecmd2 = reinterpret_cast<ert_start_kernel_cmd*>(execData);
		// Program the register map
		ecmd2->data[XFPPPre_CONTROL_ADDR_rows_in/4] = img_ht;
		ecmd2->data[XFPPPre_CONTROL_ADDR_cols_in/4] = img_wt;
		ecmd2->data[XFPPPre_CONTROL_ADDR_rows_out_resize/4] = out_height_resize;
		ecmd2->data[XFPPPre_CONTROL_ADDR_cols_out_resize/4] = out_width_resize;		
		ecmd2->data[XFPPPre_CONTROL_ADDR_rows_out/4] = org_ht;
		ecmd2->data[XFPPPre_CONTROL_ADDR_cols_out/4] = org_wt;

		int ret;
		if ((ret = xclExecBuf(handle, execHandle)) != 0) {
			std::cout << "Unable to trigger SORT, error:" << ret << std::endl;
			return ret;
		}
		do {
			ret = xclExecWait(handle, 1000);
			if (ret == 0) {
				std::cout << "SORT Task Time out, state =" << ecmd2->state << "cu_mask = " << ecmd2->cu_mask << std::endl;

			} else if (ecmd2->state == ERT_CMD_STATE_COMPLETED) {

				break;
			}
		} while (1);
	}
	catch (std::exception const& e)
	{
		std::cout << "Exception: " << e.what() << "\n";
		std::cout << "FAILED TEST\n";
		return 1;
	}	
	return 0;
}

//# Free xrt bo
void pp_dealloc(PPHandle * &pphandle) {
	xclDeviceHandle handle = pphandle->handle;	
	xclUnmapBO(handle, pphandle->paramsbuf_bo, pphandle->paramsbuf_ptr);
	xclUnmapBO(handle, pphandle->img_inp_bo, pphandle->img_inptr);	
	xclFreeBO(handle, pphandle->paramsbuf_bo);
	xclFreeBO(handle, pphandle->img_inp_bo);
	xclClose(handle);
	return;
}


int pp_kernel_init(PPHandle * &pphandle,
		char *xclbin,
		uint64_t dpu_input_phy_addr,
		float *mean,
		float input_scale)
{
	//std::cout << "Initiation: pp_kernel_init\n";
	printf("\nInitiation: pp_kernel_init\n");
	PPHandle *my_handle = new PPHandle;
	pphandle = my_handle = (PPHandle *)my_handle;

	unsigned index = 0;
	std::string halLogfile;
	unsigned cu_index = 1;

	xclDeviceHandle handle;
	uint64_t cu_base_addr = 0;
	uuid_t xclbinId;
	int first_mem = -1;
	bool ret_initXRT=0;
	bool ret_firstmem=0;
	bool ret_runkernel=0;
	bool ret_checkDevMem=0;

	if (initXRT(xclbin, index, halLogfile.c_str(), handle, cu_index, cu_base_addr, first_mem, xclbinId))
		ret_initXRT=1;
	
	if(xclOpenContext(handle, xclbinId, cu_index, true))
		throw std::runtime_error("Cannot create context");

	float params[9];
	//# Mean params
	params[0] = mean[0];
	params[1] = mean[1];
	params[2] = mean[2];
	//# Input scale
	params[3] = params[4] = params[5] = input_scale/256;
	
	//# Set to default zero
	params[6] = params[7] = params[8] = 0.0;

	const int paramsbuf_size = 9*sizeof(float);
	unsigned paramsbuf_bo = xclAllocBO(handle, paramsbuf_size, 0, 0);
	float *paramsbuf_ptr = (float*)xclMapBO(handle, paramsbuf_bo, true);
	if((paramsbuf_ptr == NULL))
		throw std::runtime_error("paramsbuf pointer is invalid\n");

	std::memcpy(paramsbuf_ptr, params, paramsbuf_size);
	//std::cout << "device write paramsbuf success\n";

	// Get & check the device memory address
	xclBOProperties p;
	uint64_t bo_paramsbuf_devAddr = !xclGetBOProperties(handle, paramsbuf_bo, &p) ? p.paddr : -1;
	if( (bo_paramsbuf_devAddr == (uint64_t)(-1)) ){
		ret_checkDevMem=1;
	}

	if(xclSyncBO(handle, paramsbuf_bo, XCL_BO_SYNC_BO_TO_DEVICE , paramsbuf_size, 0)) 
		return 1;
		
	//# creating memory for 4K image
	const int imageToDevice_size = 3840 * 2160 * 3 * sizeof(char);	
	// Allocate the device memory	
	unsigned img_inp_bo = xclAllocBO(handle, imageToDevice_size, 0, 0);
	// Create the mapping to the host memory
	unsigned char *img_inptr = (unsigned char*)xclMapBO(handle, img_inp_bo, true);
	if((img_inptr == NULL))
		throw std::runtime_error("imageToDevice pointer is invalid\n");

	uint64_t bo_input_phy_addr = !xclGetBOProperties(handle, img_inp_bo, &p) ? p.paddr : -1;
	if( (bo_input_phy_addr == (uint64_t)(-1)) ){
		ret_checkDevMem=1;
	}
	
	//thread_local static 
	unsigned execHandle = 0;
	//thread_local static 
	void *execData = nullptr;

	if(execHandle == 0) execHandle = xclAllocBO(handle, 4096, xclBOKind(0), (1<<31));
	if(execData == nullptr) execData = xclMapBO(handle, execHandle, true);

	int th1 = 127, th2 = 128;
	auto ecmd2 = reinterpret_cast<ert_start_kernel_cmd *>(execData);
	// Clear the command in case it was recycled
	size_t regmap_size = XFPPPre_CONTROL_ADDR_th2 / 4 + 1; // regmap
	std::memset(ecmd2, 0, (sizeof *ecmd2) + regmap_size);

	// Program the command packet header
	ecmd2->state = ERT_CMD_STATE_NEW;
	ecmd2->opcode = ERT_START_CU;
	ecmd2->count = 1 + regmap_size; // cu_mask + regmap
	// Program the CU mask. One CU at index 0
	ecmd2->cu_mask = 0x2;
	// Program the register map
	ecmd2->data[XFPPPre_CONTROL_ADDR_AP_CTRL] = 0x0; // ap_start
	ecmd2->data[XFPPPre_CONTROL_ADDR_img_inp / 4] = bo_input_phy_addr & 0xFFFFFFFF;
	ecmd2->data[XFPPPre_CONTROL_ADDR_img_inp / 4 + 1] = (bo_input_phy_addr >> 32) & 0xFFFFFFFF;
	ecmd2->data[XFPPPre_CONTROL_ADDR_img_out / 4] = dpu_input_phy_addr & 0xFFFFFFFF;
	ecmd2->data[XFPPPre_CONTROL_ADDR_img_out / 4 + 1] = (dpu_input_phy_addr >> 32) & 0xFFFFFFFF;
	ecmd2->data[XFPPPre_CONTROL_ADDR_params / 4] = bo_paramsbuf_devAddr & 0xFFFFFFFF;
	ecmd2->data[XFPPPre_CONTROL_ADDR_params / 4 + 1] = (bo_paramsbuf_devAddr >> 32) & 0xFFFFFFFF;
	ecmd2->data[XFPPPre_CONTROL_ADDR_th1 / 4] = th1;
	ecmd2->data[XFPPPre_CONTROL_ADDR_th2 / 4] = th2;

	my_handle->handle = handle;
	my_handle->cu_base_addr = cu_base_addr;
	my_handle->cu_index = cu_index;
	my_handle->paramsbuf_bo = paramsbuf_bo;
	my_handle->bo_paramsbuf_devAddr = bo_paramsbuf_devAddr;
	my_handle->paramsbuf_ptr = paramsbuf_ptr;
	my_handle->img_inp_bo = img_inp_bo;
	my_handle->img_inptr = img_inptr;
	my_handle->bo_input_phy_addr = bo_input_phy_addr;
	my_handle->execHandle = execHandle;
	my_handle->execData = execData;

	return 0;
}



