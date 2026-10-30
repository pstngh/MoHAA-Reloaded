/*
===========================================================================
Copyright (C) 2026 the OpenMoHAA team

This file is part of OpenMoHAA source code.

OpenMoHAA source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

OpenMoHAA source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenMoHAA source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// vk_local.h -- definitions shared by the Vulkan backend files (vk_*.c)
//
// The backend implements the interface in vk.h:
//  - vk_instance.c: loader, instance, device, swapchain
//  - vk.c: frames, command buffers, render state, drawing, clears, readback
//  - vk_image.c: textures, samplers, uploads
//  - vk_pipeline.c: pipeline cache, pipelines created before their first draw
//  - vk_shaders.c: SPIR-V generated from shaders/ by shaders/compile_shaders.sh
//
// Vulkan is loaded through SDL (SDL_Vulkan_GetVkGetInstanceProcAddr), so the
// renderer does not link against the Vulkan loader; every Vulkan function is
// called through the qvk* pointers declared here.

#pragma once

#define VK_NO_PROTOTYPES
// VkPhysicalDevicePortabilitySubsetFeaturesKHR (MoltenVK) is in vulkan_beta.h
#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>

#include "tr_local.h"

#define VK_MAX_FRAMES				2		// frames in flight, with or without the submit thread
#define VK_ACQUIRE_SEMAPHORES		( VK_MAX_FRAMES + 1 )	// see VK_AcquireNextImage in vk.c
#define VK_MAX_SWAPCHAIN_IMAGES		16
#define VK_PRESENT_FENCES			16		// presents whose fence may be pending (swapchain_maintenance1)
#define VK_MAX_RETIRED_SWAPCHAINS	8		// replaced swapchains whose presents may be pending
#define VK_MAX_TEXTURES				4096	// live texture handles
#define VK_MAX_DEPTH_READS			256		// vk_queue_depth_reads per frame
#define VK_DEPTH_READ_FRAMES		300		// the depth image stays readable this many frames after the last read

/*
====================================================================

FUNCTION POINTERS

====================================================================
*/

// loaded with vkGetInstanceProcAddr( NULL, ... )
#define VK_GLOBAL_FUNCTIONS( X ) \
	X( vkCreateInstance ) \
	X( vkEnumerateInstanceExtensionProperties ) \
	X( vkEnumerateInstanceLayerProperties )

// loaded with vkGetInstanceProcAddr( instance, ... )
#define VK_INSTANCE_FUNCTIONS( X ) \
	X( vkCreateDevice ) \
	X( vkDestroyInstance ) \
	X( vkDestroySurfaceKHR ) \
	X( vkEnumerateDeviceExtensionProperties ) \
	X( vkEnumeratePhysicalDevices ) \
	X( vkGetDeviceProcAddr ) \
	X( vkGetPhysicalDeviceFeatures ) \
	X( vkGetPhysicalDeviceFormatProperties ) \
	X( vkGetPhysicalDeviceMemoryProperties ) \
	X( vkGetPhysicalDeviceProperties ) \
	X( vkGetPhysicalDeviceQueueFamilyProperties ) \
	X( vkGetPhysicalDeviceSurfaceCapabilitiesKHR ) \
	X( vkGetPhysicalDeviceSurfaceFormatsKHR ) \
	X( vkGetPhysicalDeviceSurfacePresentModesKHR ) \
	X( vkGetPhysicalDeviceSurfaceSupportKHR )

// optional instance functions, NULL when the extension is missing
#define VK_INSTANCE_FUNCTIONS_OPTIONAL( X ) \
	X( vkCreateDebugUtilsMessengerEXT ) \
	X( vkDestroyDebugUtilsMessengerEXT ) \
	X( vkGetPhysicalDeviceFeatures2 ) \
	X( vkGetPhysicalDeviceProperties2 )

// loaded with vkGetDeviceProcAddr
#define VK_DEVICE_FUNCTIONS( X ) \
	X( vkAcquireNextImageKHR ) \
	X( vkAllocateCommandBuffers ) \
	X( vkAllocateDescriptorSets ) \
	X( vkAllocateMemory ) \
	X( vkBeginCommandBuffer ) \
	X( vkBindBufferMemory ) \
	X( vkBindImageMemory ) \
	X( vkCmdBeginRenderPass ) \
	X( vkCmdBindDescriptorSets ) \
	X( vkCmdBindIndexBuffer ) \
	X( vkCmdBindPipeline ) \
	X( vkCmdBindVertexBuffers ) \
	X( vkCmdBlitImage ) \
	X( vkCmdClearAttachments ) \
	X( vkCmdClearColorImage ) \
	X( vkCmdClearDepthStencilImage ) \
	X( vkCmdCopyBuffer ) \
	X( vkCmdCopyBufferToImage ) \
	X( vkCmdCopyImageToBuffer ) \
	X( vkCmdDispatch ) \
	X( vkCmdDraw ) \
	X( vkCmdDrawIndexed ) \
	X( vkCmdEndRenderPass ) \
	X( vkCmdPipelineBarrier ) \
	X( vkCmdPushConstants ) \
	X( vkCmdResetQueryPool ) \
	X( vkCmdSetDepthBias ) \
	X( vkCmdSetLineWidth ) \
	X( vkCmdSetScissor ) \
	X( vkCmdSetStencilCompareMask ) \
	X( vkCmdSetStencilReference ) \
	X( vkCmdSetStencilWriteMask ) \
	X( vkCmdSetViewport ) \
	X( vkCmdWriteTimestamp ) \
	X( vkCreateBuffer ) \
	X( vkCreateCommandPool ) \
	X( vkCreateComputePipelines ) \
	X( vkCreateDescriptorPool ) \
	X( vkCreateDescriptorSetLayout ) \
	X( vkCreateFence ) \
	X( vkCreateFramebuffer ) \
	X( vkCreateGraphicsPipelines ) \
	X( vkCreateImage ) \
	X( vkCreateImageView ) \
	X( vkCreatePipelineCache ) \
	X( vkCreatePipelineLayout ) \
	X( vkCreateQueryPool ) \
	X( vkCreateRenderPass ) \
	X( vkCreateSampler ) \
	X( vkCreateSemaphore ) \
	X( vkCreateShaderModule ) \
	X( vkCreateSwapchainKHR ) \
	X( vkDestroyBuffer ) \
	X( vkDestroyCommandPool ) \
	X( vkDestroyDescriptorPool ) \
	X( vkDestroyDescriptorSetLayout ) \
	X( vkDestroyDevice ) \
	X( vkDestroyFence ) \
	X( vkDestroyFramebuffer ) \
	X( vkDestroyImage ) \
	X( vkDestroyImageView ) \
	X( vkDestroyPipeline ) \
	X( vkDestroyPipelineCache ) \
	X( vkDestroyPipelineLayout ) \
	X( vkDestroyQueryPool ) \
	X( vkDestroyRenderPass ) \
	X( vkDestroySampler ) \
	X( vkDestroySemaphore ) \
	X( vkDestroyShaderModule ) \
	X( vkDestroySwapchainKHR ) \
	X( vkDeviceWaitIdle ) \
	X( vkEndCommandBuffer ) \
	X( vkFreeDescriptorSets ) \
	X( vkFreeMemory ) \
	X( vkGetBufferMemoryRequirements ) \
	X( vkGetDeviceQueue ) \
	X( vkGetFenceStatus ) \
	X( vkGetImageMemoryRequirements ) \
	X( vkGetPipelineCacheData ) \
	X( vkGetQueryPoolResults ) \
	X( vkGetSwapchainImagesKHR ) \
	X( vkMapMemory ) \
	X( vkQueuePresentKHR ) \
	X( vkQueueSubmit ) \
	X( vkQueueWaitIdle ) \
	X( vkResetFences ) \
	X( vkUnmapMemory ) \
	X( vkUpdateDescriptorSets ) \
	X( vkWaitForFences )

// optional device functions, NULL when the extension is not enabled;
// vkCmdSetLineStippleEXT and vkReleaseSwapchainImagesEXT are loaded under
// the name of the enabled extension (VK_KHR_line_rasterization or
// VK_EXT_line_rasterization, VK_KHR_swapchain_maintenance1 or
// VK_EXT_swapchain_maintenance1)
#define VK_DEVICE_FUNCTIONS_OPTIONAL( X ) \
	X( vkCmdSetLineStippleEXT ) \
	X( vkCreateRenderPass2KHR ) \
	X( vkReleaseSwapchainImagesEXT ) \
	X( vkSetHdrMetadataEXT )

#define VK_DECLARE_FUNCTION( name ) extern PFN_##name q##name;
VK_GLOBAL_FUNCTIONS( VK_DECLARE_FUNCTION )
VK_INSTANCE_FUNCTIONS( VK_DECLARE_FUNCTION )
VK_INSTANCE_FUNCTIONS_OPTIONAL( VK_DECLARE_FUNCTION )
VK_DEVICE_FUNCTIONS( VK_DECLARE_FUNCTION )
VK_DEVICE_FUNCTIONS_OPTIONAL( VK_DECLARE_FUNCTION )
#undef VK_DECLARE_FUNCTION

/*
====================================================================

TYPES

====================================================================
*/

// a buffer with its own memory, persistently mapped when host visible
typedef struct {
	VkBuffer		buffer;
	VkDeviceMemory	memory;
	VkDeviceSize	size;
	byte			*mapped;
} vkBuffer_t;

// storage of a texture; replaced when vk_create_texture is called again
typedef struct {
	VkImage			image;
	VkDeviceMemory	memory;			// a block of vk_image.c or memory of its own
	int				memoryBlock;	// index of the block, -1 for memory of its own
	VkDeviceSize	memoryOffset, memorySize;
	VkImageView		view;
	VkDescriptorSet	descriptorSet;	// combined image sampler, set 0 or 1
	VkDescriptorPool descriptorPool;	// the pool descriptorSet came from
	VkFormat		format;
	int				width, height;
	int				mipLevels;
	qboolean		compressed;
	qboolean		rgb;			// alpha samples as 1 (A = ONE swizzle)
	int				wrapX, wrapY;	// WRAP_REPEAT or WRAP_CLAMP

	// layout tracking for uploads (see vk_image.c); between command buffers
	// every image is in SHADER_READ_ONLY_OPTIMAL
	qboolean		uploadPending;	// in TRANSFER_DST_OPTIMAL in the upload command buffer
	qboolean		mainPending;	// in TRANSFER_DST_OPTIMAL in the main command buffer
	uint32_t		uploadMips;		// mip levels written since uploadPending was set
	uint32_t		mainMips;		// mip levels written since mainPending was set
	uint64_t		lastUseSegment;	// segment of the main command buffer that last referenced it
} vkTexture_t;

// a slot of the texture table, addressed by image_t::texnum
typedef struct {
	unsigned int	handle;			// 0 when the slot is free
	vkTexture_t		*texture;
} vkTextureSlot_t;

typedef enum {
	VK_DESTROY_BUFFER,
	VK_DESTROY_IMAGE,
	VK_DESTROY_IMAGE_VIEW,
	VK_DESTROY_MEMORY,
	VK_DESTROY_MEMORY_RANGE,		// a range of a texture memory block (vk_image.c)
	VK_DESTROY_DESCRIPTOR_SET,
	VK_DESTROY_FRAMEBUFFER
} vkDestroyType_t;

// a resource released once the frame that may still use it has completed
typedef struct {
	vkDestroyType_t	type;
	uint64_t		handle;			// the object; the block index for VK_DESTROY_MEMORY_RANGE
	uint64_t		pool;			// descriptor pool for VK_DESTROY_DESCRIPTOR_SET, offset for VK_DESTROY_MEMORY_RANGE
	uint64_t		size;			// VK_DESTROY_MEMORY_RANGE
} vkDeferredDestroy_t;

typedef enum {
	VK_TARGET_NONE,				// not chosen yet: nothing drawn or read in the frame
	VK_TARGET_OFFSCREEN,		// vk.colorImage, blitted to the swapchain at present
	VK_TARGET_SWAPCHAIN			// the acquired swapchain image
} vkTarget_t;

// a light of vk_add_dlights in the frame's dlight buffer (shaders/generic.frag)
typedef struct {
	float	origin[3];
	float	invRadius;			// 1 / radius
	float	color[3];			// vkDlight_t::color / scale
	float	scale;
} vkDlightGPU_t;

// how the output pass (r_hdrOutput, vk_output.c) encodes the frame for the
// swapchain
typedef enum {
	VK_HDR_SDR,		// an 8-bit SDR swapchain: the frame clamped at 1
	VK_HDR_PQ,		// HDR10: BT.2020 primaries, ST 2084 (PQ)
	VK_HDR_SCRGB	// scRGB: BT.709 primaries, linear, 1 = 80 nits
} vkHdrMode_t;

// the output pass's push constants (shaders/output.frag), made on the main
// thread at the end of the frame for the submit thread
typedef struct {
	int		mode;			// vkHdrMode_t of the swapchain
	int		test;			// r_hdrOutputTest
	float	paperWhite;		// nits of the frame's 1.0
	float	peak;			// the display's peak over paper white, in linear light (1 in SDR)
	float	invExtent[2];	// 1 / the swapchain's size
	float	black;			// r_hdrBlack; 0 with an SDR swapchain but for the HDR look's calibration
	float	sdrBox;			// the HDR look's peak calibration screen: the box, linear (VK_OutputSdrBox)
} vkOutputPush_t;

typedef struct {
	VkCommandPool	commandPool;
	VkCommandBuffer	uploadCmd;		// texture uploads that need no ordering with draws, submitted first
	VkCommandBuffer	mainCmd;		// everything else, in the order the renderer issued it
	VkCommandBuffer	presentCmd;		// copy to the swapchain image (recorded by the submit thread with r_vkSubmitThread)
	VkFence			fence;			// signaled when every submission of the frame has completed
	VkQueryPool		queryPool;		// VK_NUM_TIMESTAMPS timestamps, when the queue has them
	uint32_t		timestampsWritten;	// bits of the VK_TS_* recorded in this frame

	vkBuffer_t		vertexBuffer;	// vertexes and indexes of this frame
	VkDeviceSize	vertexOffset;
	vkBuffer_t		stagingBuffer;	// texture upload data of this frame
	VkDeviceSize	stagingOffset;
	vkBuffer_t		dlightBuffer;	// lights of vk_add_dlights (vkDlightGPU_t), set 2 of the shaders
	VkDescriptorSet	dlightSet;		// set 2: the dlight buffer, the skin data and this frame's skin data
	int				numDlights;
	// the lists of more than 32 lights, at their place in the dlight buffer
	// (which is write-combined memory): the draws whose lights span more
	// than 32 of them get lists of their own (VK_DlightSubList)
	vkDlightGPU_t	*dlightCopy;
	vkBuffer_t		skinBuffer;		// frame data of the skinned draws (vk_alloc_skin_frame)
	int				skinWords;

	vkDeferredDestroy_t	*destroyList;
	int				numDestroy, maxDestroy;

	// depth reads (vk_queue_depth_reads) recorded in this frame, and the
	// results of the frame that used this slot before, copied when the slot
	// is reused
	uint64_t		serial;			// vk.frameSerial of the frame recorded in this slot
	vkBuffer_t		depthReadBuffer;	// one 4-byte texel per read
	int				numDepthReads;
	byte			depthReadInside[VK_MAX_DEPTH_READS];	// qfalse: outside of the framebuffer, reads 0
	uint64_t		resultSerial;	// frame whose results are in depthResults, 0 for none
	int				numDepthResults;
	float			depthResults[VK_MAX_DEPTH_READS];

	vkOutputPush_t	outputPush;		// the output pass of this frame (r_hdrOutput)
} vkFrame_t;

// counters for the vkinfo command (vk.c), accumulated since the device was
// created; vkinfo prints what changed since its last call
typedef struct {
	uint64_t	frames;				// vk_end_frame calls
	uint64_t	renderPasses;		// render pass instances begun
	uint64_t	submits;			// vkQueueSubmit calls
	uint64_t	flushes;			// mid-frame submissions that waited for the GPU
	uint64_t	draws;
	uint64_t	staticDraws;		// draws from the static geometry (vk_set_static_geometry)
	uint64_t	effectsDraws;		// of those, with the effects shader (drawGeometry_t::staticFx)
	uint64_t	skinDraws;			// skinned draws (drawGeometry_t::isSkinned)
	uint64_t	pipelineBinds;
	uint64_t	descriptorBinds;	// descriptor sets bound
	uint64_t	vertexBytes;		// copied into the vertex buffers
	uint64_t	stagingBytes;		// texture data copied into staging buffers
	uint64_t	mainUploads;		// uploads recorded in order with the draws (they end the render pass)
	uint64_t	textures;			// texture storages created
	uint64_t	pipelines;			// pipelines created (by draws and by vk_prebuild_pipeline)
	uint64_t	prebuiltPipelines;	// pipelines created before a draw needed them (vk_prebuild_pipeline, recorded keys)
	uint64_t	skinUploads;		// vk_add_skin_data calls that added words
	uint64_t	skinBytes;			// bytes they added
	uint64_t	allocations;		// vkAllocateMemory calls
	uint64_t	depthTargets;		// depth images replaced to become readable or not (VK_UpdateDepthTarget)
	uint64_t	swapchains;			// swapchains created (VK_CreateSwapchain)
	uint64_t	vertexGrowth;		// bytes of the frame vertex buffers created when one was full (VK_VertexAlloc)
	// microseconds spent in
	uint64_t	waitUs;				// waits for frame fences, mid-frame flushes included
	uint64_t	flushUs;			// waits of the mid-frame flushes (VK_FlushAndWait)
	uint64_t	submitUs;			// vkQueueSubmit
	uint64_t	acquireUs;			// vkAcquireNextImageKHR, in a frame (before presenting it)
	uint64_t	acquireAheadUs;		// vkAcquireNextImageKHR right after a present, for the next frame
	uint64_t	frameToPresentUs;	// from the start of a frame's recording to its present
	uint64_t	presentUs;			// vkQueuePresentKHR
	uint64_t	pipelineUs;			// vkCreateGraphicsPipelines
	uint64_t	prebuiltUs;			// vkCreateGraphicsPipelines of those
	uint64_t	allocationUs;		// vkAllocateMemory
	uint64_t	textureUs;			// creating texture storages (image, memory, view, descriptor set)
	// GPU time from timestamps, of the frames that presented
	uint64_t	gpuFrames;
	uint64_t	gpuMainUs;			// the main command buffer (all segments of the frame)
	uint64_t	gpuPresentUs;		// the copy to the swapchain image
	uint64_t	gpuFrameUs;			// start of the main command buffer to the end of the copy
	// the submit thread (r_vkSubmitThread, vk_submit.c): the main thread's
	// waits for it, and its own times, taken when the main thread joins a
	// frame; submits, submitUs, acquireUs and presentUs above are the main
	// thread's only
	uint64_t	threadWaitUs;		// the main thread waited for the thread to finish a frame
	uint64_t	threadFrames;		// frames the thread finished
	uint64_t	threadStartUs;		// from the hand-off of a frame to the thread starting it
	uint64_t	threadSubmits;		// vkQueueSubmit calls of the thread
	uint64_t	threadSubmitUs;
	uint64_t	threadAcquireUs;	// vkAcquireNextImageKHR on the thread
	uint64_t	threadPresents;		// vkQueuePresentKHR calls of the thread
	uint64_t	threadPresentUs;
	uint64_t	threadCopies;		// frames drawn offscreen and copied by the thread (no image was acquired at the first render pass)
} vkStats_t;

// a fence that a present signals (VK_KHR/EXT_swapchain_maintenance1) once
// its wait semaphore can be reused or destroyed; the fences of the presents
// to one swapchain signal in present order
typedef struct {
	VkFence			fence;
	qboolean		pending;		// given to a present, not seen signaled since
	uint32_t		uses;			// presents it was given to
} vkPresentFence_t;

// a swapchain replaced by a new one (or destroyed while minimized) and the
// semaphores its presents waited for, destroyed once its presents are done
// (VK_DestroyRetiredSwapchains in vk_instance.c)
typedef struct {
	VkSwapchainKHR	swapchain;
	uint32_t		numSemaphores;
	VkSemaphore		semaphores[VK_MAX_SWAPCHAIN_IMAGES];
	int				lastFence;		// vk.presentFences slot of its last present, -1 for none
	uint32_t		lastFenceUse;	// the slot's uses after that present
} vkRetiredSwapchain_t;

// GPU timestamps of a frame
#define VK_TS_MAIN_START		0
#define VK_TS_MAIN_END			1
#define VK_TS_PRESENT_START		2
#define VK_TS_PRESENT_END		3
#define VK_NUM_TIMESTAMPS		4

typedef struct {
	// instance and device
	VkInstance			instance;
	VkDebugUtilsMessengerEXT debugMessenger;
	VkSurfaceKHR		surface;
	VkPhysicalDevice	physicalDevice;
	VkPhysicalDeviceProperties deviceProperties;
	VkPhysicalDeviceFeatures enabledFeatures;
	qboolean			depthClipControl;	// VK_EXT_depth_clip_control: GL clip space depth (z in [-w, w])
	qboolean			bresenhamLines;		// VK_KHR/EXT_line_rasterization: GL's diamond-exit line rule
	qboolean			lineStipple;		// stippledBresenhamLines
	qboolean			depthStencilResolve;	// VK_KHR_depth_stencil_resolve (and create_renderpass2), for multisampling
	// VK_KHR/EXT_swapchain_maintenance1: presents signal fences (vk.presentFences)
	qboolean			presentFences;
	// imageViewFormatSwizzle (always outside of the portability subset): RGB
	// textures sample alpha 1 from an A = ONE view swizzle, otherwise the
	// fragment shader sets it (VK_PIPE_TEX*_RGB with every texture env)
	qboolean			viewSwizzle;
	// outside of the portability subset (which requires strides of at
	// least minVertexInputBindingStrideAlignment): constant colors and unused
	// texture coordinates are read with a binding stride of 0, otherwise per
	// instance (see vk_pipeline.c)
	qboolean			strideZero;
	int					fallbacks;			// r_vkFallbacks: VK_FALLBACK_* forced at startup
	VkPhysicalDeviceMemoryProperties memoryProperties;
	VkDevice			device;
	uint32_t			queueFamily;
	uint32_t			timestampValidBits;	// 0: the queue has no timestamps
	VkQueue				queue;

	// r_vkSubmitThread: a thread submits and presents the frames and
	// acquires the swapchain images, and copies a frame drawn offscreen when
	// no image was acquired by its first render pass (vk_submit.c)
	qboolean			submitThread;

	// swapchain
	VkSwapchainKHR		swapchain;
	VkFormat			swapchainFormat;
	VkExtent2D			swapchainExtent;
	VkPresentModeKHR	presentMode;
	uint32_t			swapchainImageCount;
	VkImage				swapchainImages[VK_MAX_SWAPCHAIN_IMAGES];
	// the semaphores the presents wait for, one per image of this swapchain:
	// the one of an image is signaled again once the image has been acquired
	// again and the acquire semaphore waited for, which guarantees that the
	// image's previous present is done with it; they go with the swapchain
	// (vkRetiredSwapchain_t)
	VkSemaphore			presentSemaphores[VK_MAX_SWAPCHAIN_IMAGES];
	uint32_t			numPresentSemaphores;
	int					lastPresentFence;		// vk.presentFences slot of the last present, -1 for none
	uint32_t			lastPresentFenceUse;
	VkImageUsageFlags	swapchainUsage;
	// frames are drawn into the swapchain images directly when they can be
	// color attachments and readback sources of the render target's format
	// (see VK_SelectTarget in vk.c)
	qboolean			swapchainDirect;
	VkImageView			swapchainViews[VK_MAX_SWAPCHAIN_IMAGES];
	VkFramebuffer		swapchainFramebuffers[VK_MAX_SWAPCHAIN_IMAGES];
	qboolean			swapchainOutOfDate;
	int					swapchainDrawableWidth;		// window size the swapchain was created for
	int					swapchainDrawableHeight;
	vkRetiredSwapchain_t retiredSwapchains[VK_MAX_RETIRED_SWAPCHAINS];
	int					numRetiredSwapchains;
	// present fences (vk.presentFences): slots created when every other one
	// is pending; the next present's is reset after the present before it
	// (VK_PreparePresentFence), so nothing waits between a frame's input and
	// its present
	vkPresentFence_t	presentFenceSlots[VK_PRESENT_FENCES];
	int					numPresentFences;
	int					nextPresentFence;		// unsignaled, for the next present; -1 for none
	int					presentFenceScan;		// the slot after the last one taken: usually the oldest

	// render target of glConfig.vidWidth x vidHeight: the color target of a
	// frame is the swapchain image it will present (vk.target), or, when the
	// swapchain doesn't have that size (a window resize before the
	// vid_restart) or can't be drawn to, the offscreen color image, blitted
	// to the swapchain image at present. The depth/stencil image is always
	// offscreen. With multisampling (r_ext_multisample) the draws go to the
	// multisampled images; every render pass resolves the color into the
	// color target, the depth/stencil buffer is resolved into the depth image
	// before readbacks
	int					width, height;
	VkSampleCountFlagBits samples;
	VkFormat			colorFormat;
	VkFormat			depthFormat;
	VkImageAspectFlags	depthAspect;	// depth, and stencil when the format has it (see VK_CreateRenderTargets)
	VkImage				colorImage;
	VkDeviceMemory		colorMemory;
	VkImageView			colorView;
	VkImage				depthImage;
	VkDeviceMemory		depthMemory;
	VkImageView			depthView;
	// the depth image can be copied from (TRANSFER_SRC): always with
	// multisampling, otherwise only while depth or stencil reads are wanted
	// (VK_UpdateDepthTarget in vk.c)
	qboolean			depthReadable;
	// color clears to colors other than 0 and 1 are drawn as a quad (RADV,
	// see VK_DrawClearColor in vk.c)
	qboolean			drawColorClears;
	uint64_t			depthReadSerial;	// frame in which depth or stencil reads were last wanted, 0 for never
	VkImage				msColorImage, msDepthImage;
	VkDeviceMemory		msColorMemory, msDepthMemory;
	VkImageView			msColorView, msDepthView;
	VkRenderPass		renderPass;
	VkFramebuffer		framebuffer;
	VkRenderPass		depthResolvePass;	// multisampling: resolves the depth/stencil buffer for readbacks
	VkFramebuffer		depthResolveFramebuffer;

	// shared objects
	VkDescriptorSetLayout textureSetLayout;
	VkDescriptorSetLayout dlightSetLayout;	// set 2: the frame's dlight buffer, the skin data and the frame's skin data
	VkDescriptorPool	dlightPool;
	VkPipelineLayout	pipelineLayout;
	VkShaderModule		vertexShader;
	VkShaderModule		vertexShaderClip;	// writes gl_ClipDistance (shaderClipDistance only)
	VkShaderModule		skinVertexShader;	// skinned draws (shaders/skinned.vert)
	VkShaderModule		skinVertexShaderClip;
	VkShaderModule		effectsVertexShader;	// static geometry with effects (shaders/effects.vert)
	VkShaderModule		effectsVertexShaderClip;
	VkShaderModule		fragmentShader;
	VkPipelineCache		pipelineCache;

	// frames
	vkFrame_t			frames[VK_MAX_FRAMES];
	int					frameIndex;
	vkFrame_t			*frame;			// current frame, NULL between frames
	vkTarget_t			target;			// color target of the current frame, chosen at its first render pass
	VkImage				targetImage;
	VkFramebuffer		targetFramebuffer;
	uint32_t			targetIndex;	// swapchain image for VK_TARGET_SWAPCHAIN
	qboolean			acquireWaitPending;	// the next submission waits for the held image's semaphore

	// the swapchain image acquired for the next present (VK_AcquireNextImage),
	// usually right after the previous present
	qboolean			heldImage;
	uint32_t			heldIndex;
	int					heldSemaphore;	// index in acquireSemaphores
	// a ring, used by the submit thread when it runs (vk_submit.c)
	VkSemaphore			acquireSemaphores[VK_ACQUIRE_SEMAPHORES];
	uint64_t			acquireWaitSerial[VK_ACQUIRE_SEMAPHORES];	// frame whose submission waited for it, 0 for none
	int					nextAcquireSemaphore;
	uint64_t			frameStartUs;	// when the frame being recorded started
	uint64_t			lastPresentUs;	// when vkQueuePresentKHR was last called (vk_framelog.c)
	uint64_t			frameSerial;	// incremented when a frame starts, the first frame is 1
	qboolean			uploadCmdRecording;
	qboolean			renderPassActive;
	uint64_t			segment;		// incremented each time the main command buffer is submitted
	VkFence				lastSubmitFence;	// VK_NULL_HANDLE when nothing is pending

	vkBuffer_t			readbackBuffer;

	vkStats_t			stats;
	vkStats_t			statsReported;	// stats at the last vkinfo
	int					liveAllocations;	// device memory objects
} vkInstance_t;

extern vkInstance_t vk;

/*
====================================================================

FUNCTIONS

====================================================================
*/

// r_vkFallbacks: what the device has is ignored, to test the fallbacks
#define VK_FALLBACK_VIEW_SWIZZLE	1	// no imageViewFormatSwizzle
#define VK_FALLBACK_STRIDE_ZERO		2	// no binding stride of 0 (as with the portability subset)
#define VK_FALLBACK_LINES			4	// no VK_KHR/EXT_line_rasterization
#define VK_FALLBACK_PRESENT_FENCES	8	// no VK_KHR/EXT_swapchain_maintenance1

// vk_instance.c
void		VK_CreateDevice( void );
void		VK_DestroyDevice( void );
void		VK_CreateSwapchain( void );
void		VK_DestroySwapchain( void );
void		VK_DestroyRetiredSwapchains( qboolean wait );
void		VK_PreparePresentFence( void );
VkResult	VK_TryPreparePresentFence( void );
void		VK_PresentFenceUsed( void );
void		VK_DestroyPresentFences( void );
const char	*VK_SwapchainPresentModeName( void );
void		VK_GetDrawableSize( int *width, int *height );
const char	*VK_ResultString( VkResult result );
uint64_t	VK_Microseconds( void );
void		VK_Check( VkResult result, const char *what );
#define		VK_CHECK( call ) VK_Check( ( call ), #call )

// vk.c
void		VK_CreateSwapchainTargets( void );
void		VK_ReleaseHeldImage( void );
void		VK_DestroySwapchainTargets( void );
uint32_t	VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred );
VkDeviceMemory VK_AllocateMemory( VkDeviceSize size, uint32_t memoryType );
void		VK_FreeMemory( VkDeviceMemory memory );
void		VK_CreateBuffer( vkBuffer_t *buffer, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred );
void		VK_DestroyBuffer( vkBuffer_t *buffer );
void		VK_DeferDestroy( vkDestroyType_t type, uint64_t handle, uint64_t pool );
void		VK_DeferDestroyRange( int block, VkDeviceSize offset, VkDeviceSize size );
void		VK_BeginFrameIfNeeded( void );
void		VK_EndRenderPass( void );
void		VK_FlushAndWait( void );
VkCommandBuffer VK_GetUploadCommandBuffer( void );
qboolean	VK_FormatHasFeatures( VkFormat format, VkFormatFeatureFlags features );
void		VK_CreateImage( VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect,
				int width, int height, int mipLevels, VkImage *image, VkDeviceMemory *memory, VkImageView *view );
void		VK_DestroyTargetImage( VkImage *image, VkDeviceMemory *memory, VkImageView *view );
void		VK_ImageBarrier( VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
				VkImageLayout oldLayout, VkImageLayout newLayout,
				VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
				VkPipelineStageFlags dstStage, VkAccessFlags dstAccess );
VkResult	VK_RecordPresentCopy( vkFrame_t *frame, VkCommandBuffer cmd, VkImage image );
VkPipelineStageFlags VK_PresentCopyStage( void );

/*
====================================================================

OUTPUT PASS (vk_output.c)

r_hdrOutput (latched) with VK_EXT_swapchain_colorspace: the frame's color
target is RGBA16F, holding the values the game draws (gamma encoded, 1.0 =
paper white; the 3D scene's expansion goes above 1, temporal_composite.frag),
and every frame goes to the swapchain through the output pass instead of
the blit: clamped at 1 for an SDR swapchain, encoded as PQ (HDR10) or scRGB
when the surface offers it and the window is in HDR (SDL)

====================================================================
*/

typedef struct {
	qboolean		enabled;		// r_hdrOutput latched on, and the instance has VK_EXT_swapchain_colorspace
	// r_hdrOutput 2 (the HDR look in SDR): the output pass exists for the
	// calibration screens only; the other frames go to the swapchain as
	// without HDR output
	qboolean		sdrPass;
	qboolean		metadata;		// VK_EXT_hdr_metadata is enabled
	vkHdrMode_t		mode;			// the swapchain's
	qboolean		windowHdr;		// SDL's HDR state of the window when the swapchain was chosen
	qboolean		listed;			// the surface's HDR formats were printed
	const char		*reason;		// why the swapchain is SDR (printed when it changes)
	float			paperWhite;		// nits, this frame's (VK_OutputUpdate)
	float			peakNits;
	float			metadataWhite, metadataPeak;	// what vkSetHdrMetadataEXT was given last, 0 for nothing
	qboolean		sdrFrame;		// the frame is read back (screenshot, video): no expansion
	qboolean		noSceneNote;	// the note that r_temporal 0 has no expansion was printed

	VkFormat		passFormat;		// of the render pass and the pipeline
	VkRenderPass	renderPass;
	VkPipeline		pipeline;
	VkPipelineLayout	layout;
	VkDescriptorSetLayout	setLayout;
	VkDescriptorPool	pool;
	VkDescriptorSet	set;
	VkImageView		setView;		// the color view the set holds
	VkSampler		sampler;
	VkShaderModule	vertexShader, fragmentShader;
	VkImageView		views[VK_MAX_SWAPCHAIN_IMAGES];
	VkFramebuffer	framebuffers[VK_MAX_SWAPCHAIN_IMAGES];
} vkOutput_t;

extern vkOutput_t	vko;

void		VK_OutputRegisterCvars( void );
qboolean	VK_OutputWanted( void );
qboolean	VK_OutputSdrPassWanted( void );
qboolean	VK_OutputPassAvailable( void );
qboolean	VK_OutputNeedsOffscreen( void );
qboolean	VK_OutputDrawsFrame( const vkFrame_t *frame );
qboolean	VK_OutputChooseFormat( const VkSurfaceFormatKHR *formats, uint32_t count, VkSurfaceFormatKHR *chosen );
void		VK_OutputSwapchainCreated( void );
void		VK_OutputUpdate( void );
void		VK_OutputFillPush( vkOutputPush_t *push );
void		VK_OutputExpansion( float *params );
float		VK_OutputSdrBox( void );
void		VK_OutputCreateTargets( void );
void		VK_OutputDestroyTargets( void );
void		VK_OutputRecord( const vkFrame_t *frame, VkCommandBuffer cmd, VkImage image );
void		VK_OutputShutdown( void );
float		VK_HalfToFloat( uint16_t h );

// vk_submit.c: the submit thread (r_vkSubmitThread)
qboolean	VK_StartSubmitThread( void );
void		VK_StopSubmitThread( void );
void		VK_QueueSubmitJob( vkFrame_t *frame, const VkCommandBuffer *cmds, uint32_t numCmds, qboolean present, int image, VkSemaphore acquired );
void		VK_JoinSubmitJob( int slot );
void		VK_CollectSubmitJobs( void );
void		VK_DrainSubmitThread( void );
qboolean	VK_TakeSubmitThreadImage( uint32_t *index, int *semaphore );
void		VK_ReclaimSubmitThreadImage( void );
qboolean	VK_DeferThreadMessage( const char *text );

// vk_image.c
void		VK_InitTextures( void );
void		VK_ShutdownTextures( void );
void		VK_ShutdownTextureObjects( void );
vkTexture_t	*VK_GetTexture( unsigned int handle );
vkTexture_t	*VK_DefaultTexture( void );
void		VK_FinishUploads( VkCommandBuffer cmd, qboolean upload );
void		VK_FreeMemoryRange( int block, VkDeviceSize offset, VkDeviceSize size );
void		VK_MemoryBlockInfo( int *numBlocks, VkDeviceSize *size, VkDeviceSize *used );
void		*VK_StagingAlloc( VkDeviceSize size, VkDeviceSize alignment, VkBuffer *buffer, VkDeviceSize *offset );

// vk_framelog.c
extern qboolean	vk_frameLog;	// r_vkFrameLog: the frame log records
void		VK_FrameLogGpu( uint64_t serial, uint64_t mainStart, uint64_t mainEnd, uint64_t frameEnd, uint64_t mask, double usPerTick );
void		VK_FrameLogThread( uint64_t serial, uint64_t presentAt, uint64_t startUs, uint64_t submitUs, uint64_t acquireUs, uint64_t presentUs );

// vk_pipeline.c
typedef struct {
	uint64_t	bits;			// VK_PIPE_* fields
	uint32_t	alphaRef;		// float bits of the alpha test reference
} vkPipelineKey_t;

void		VK_InitPipelines( void );
VkShaderModule VK_CreateShaderModule( const uint32_t *code, size_t size );
void		VK_ShutdownPipelines( void );
VkPipeline	VK_GetPipeline( const vkPipelineKey_t *key );
qboolean	VK_PrebuildPipeline( const vkPipelineKey_t *key );
int			VK_NumPipelines( void );
void		VK_PipelineCounts( int *total, int *prebuilt, int *prebuiltUsed );

// pipeline key fields
#define VK_PIPE_SRCBLEND_SHIFT		0		// GLS_SRCBLEND_* value, 4 bits
#define VK_PIPE_DSTBLEND_SHIFT		4		// GLS_DSTBLEND_* value >> 4, 4 bits
#define VK_PIPE_DEPTHTEST			( 1ull << 8 )
#define VK_PIPE_DEPTHWRITE			( 1ull << 9 )
#define VK_PIPE_DEPTHFUNC_SHIFT		10		// vkDepthFunc_t, 2 bits
#define VK_PIPE_POLYMODE_LINE		( 1ull << 12 )
#define VK_PIPE_CULL_SHIFT			13		// cullFace_t, 2 bits
#define VK_PIPE_LINES				( 1ull << 15 )
#define VK_PIPE_COLORMASK_SHIFT		16		// RGB write mask, 3 bits
#define VK_PIPE_ATEST_SHIFT			19		// 0 off, 1 >, 2 <, 3 >=, 2 bits
#define VK_PIPE_DEPTHBIAS			( 1ull << 21 )
#define VK_PIPE_COLOR_ARRAY			( 1ull << 22 )
#define VK_PIPE_TEXENV0_SHIFT		23		// texEnv_t, 0 when unit 0 is off, 3 bits
#define VK_PIPE_TEXENV1_SHIFT		26		// texEnv_t, 0 when unit 1 is off, 3 bits
#define VK_PIPE_TEX0_RGB			( 1ull << 29 )
#define VK_PIPE_TEX1_RGB			( 1ull << 30 )
#define VK_PIPE_STENCIL				( 1ull << 31 )
#define VK_PIPE_STENCILFUNC_SHIFT	32		// stencilFunc_t, 1 bit
#define VK_PIPE_STENCILOP_SHIFT		33		// 3 x stencilOp_t, 2 bits each
#define VK_PIPE_FOG					( 1ull << 39 )	// linear fog
#define VK_PIPE_CLIPPLANE			( 1ull << 40 )	// user clip plane
#define VK_PIPE_STIPPLE				( 1ull << 41 )	// line stipple (lines and polygon edges)
#define VK_PIPE_DLIGHT_SHIFT		42		// unit + 1 whose texture gets the dynamic lights, 0 for none, 2 bits
#define VK_PIPE_DLIGHT_BACKS		( 1ull << 44 )	// no dynamic light from behind the triangle
#define VK_PIPE_SKINNED				( 1ull << 45 )	// skinned draw: shaders/skinned.vert, no vertex input
#define VK_PIPE_EFFECTS				( 1ull << 46 )	// static geometry with effects: shaders/effects.vert, normals in binding 4
#define VK_PIPE_DEPTHCLAMP			( 1ull << 47 )	// depth clamp instead of near/far clipping (vk_set_depth_clamp)
#define VK_PIPE_TEMPORAL			( 1ull << 48 )	// the temporal path's scene (vk_temporal.c): its render pass, layout and shaders
#define VK_PIPE_NORMALS				( 1ull << 49 )	// temporal, writes the normals buffer, not skinned or effects: normals per vertex in binding 4 (else a constant 0)
#define VK_PIPE_MOTION				( 1ull << 50 )	// temporal, writes the normals buffer, and the motion vectors: draws what moves

typedef enum {
	VK_DEPTHFUNC_LEQUAL,
	VK_DEPTHFUNC_EQUAL,
	VK_DEPTHFUNC_ALWAYS
} vkDepthFunc_t;

// push constants of shaders/generic.*: the vertex stage reads the first
// VK_PUSH_VERTEX_SIZE bytes, the fragment stage the rest
typedef struct {
	float	mvp[16];			// projection * modelview
	float	eyeZ[4];			// row 2 of the modelview (eye space z)
	float	clipPlane[4];		// user clip plane in object space, 0 when off
	float	fogColor[4];
	float	fogParams[2];		// -1 / (end - start), end / (end - start)
	uint32_t	dlightBase;		// first light of the list in the frame's dlight buffer, and VK_DLIGHT_COUNT
	uint32_t	dlightMask;		// the lights of the 32 from the base that the draw adds, or their number
} vkPushConstants_t;

#define VK_MAX_FRAME_DLIGHTS	8192	// per frame: 128 lists of 64 lights
// vkPushConstants_t::dlightBase: dlightMask is the number of lights of the
// list from the base (the other bits), not a mask (VK_DlightSubList)
#define VK_DLIGHT_COUNT			0x80000000u

#define VK_SKIN_DATA_WORDS		( 8 * 1024 * 1024 )	// skin data, 32 MB
#define VK_SKIN_FRAME_WORDS		( 512 * 1024 )		// frame data of the skinned draws, 2 MB per frame

#define VK_PUSH_VERTEX_SIZE		( (uint32_t)offsetof( vkPushConstants_t, fogColor ) )
#define VK_PUSH_FRAGMENT_SIZE	( (uint32_t)( sizeof( vkPushConstants_t ) - VK_PUSH_VERTEX_SIZE ) )

// the push constants the temporal path's vertex shaders add after
// vkPushConstants_t (shaders/generic.vert)
typedef struct {
	float	prevMvp[16];		// the last frame's projection (unjittered) * modelview
	float	mvRow0[4];			// rows 0 and 1 of the modelview (row 2 is eyeZ)
	float	mvRow1[4];
	float	jitter[4];			// xy: the projection's jitter in NDC, z: VK_TEMPORAL_CLASS_*, w: 0
} vkTemporalPush_t;

#define VK_TEMPORAL_PUSH_OFFSET	( (uint32_t)sizeof( vkPushConstants_t ) )
#define VK_TEMPORAL_PUSH_END	( VK_TEMPORAL_PUSH_OFFSET + (uint32_t)sizeof( vkTemporalPush_t ) )

// the class of a pixel in the normals buffer (its 2 alpha bits; see
// shaders/temporal_common.glsl)
#define VK_TEMPORAL_CLASS_NONE		0	// nothing that hides what is behind it was drawn there
#define VK_TEMPORAL_CLASS_SURFACE	1
#define VK_TEMPORAL_CLASS_WEAPON	2	// the view weapon (depth hack range)
#define VK_TEMPORAL_CLASS_SKY		3	// the sky (depth range [1, 1])

// vk_shaders.c
extern const uint32_t	vk_spirv_generic_vert[];
extern const size_t		vk_spirv_generic_vert_size;
extern const uint32_t	vk_spirv_generic_clip_vert[];
extern const size_t		vk_spirv_generic_clip_vert_size;
extern const uint32_t	vk_spirv_generic_frag[];
extern const size_t		vk_spirv_generic_frag_size;
extern const uint32_t	vk_spirv_skinned_vert[];
extern const size_t		vk_spirv_skinned_vert_size;
extern const uint32_t	vk_spirv_skinned_clip_vert[];
extern const size_t		vk_spirv_skinned_clip_vert_size;
extern const uint32_t	vk_spirv_effects_vert[];
extern const size_t		vk_spirv_effects_vert_size;
extern const uint32_t	vk_spirv_effects_clip_vert[];
extern const size_t		vk_spirv_effects_clip_vert_size;

// vk_shaders_hdr.c
extern const uint32_t	vk_spirv_output_frag[];
extern const size_t		vk_spirv_output_frag_size;

// vk_shaders_temporal.c
extern const uint32_t	vk_spirv_generic_temporal_vert[];
extern const size_t		vk_spirv_generic_temporal_vert_size;
extern const uint32_t	vk_spirv_generic_temporal_clip_vert[];
extern const size_t		vk_spirv_generic_temporal_clip_vert_size;
extern const uint32_t	vk_spirv_generic_temporal_frag[];
extern const size_t		vk_spirv_generic_temporal_frag_size;
extern const uint32_t	vk_spirv_skinned_temporal_vert[];
extern const size_t		vk_spirv_skinned_temporal_vert_size;
extern const uint32_t	vk_spirv_skinned_temporal_clip_vert[];
extern const size_t		vk_spirv_skinned_temporal_clip_vert_size;
extern const uint32_t	vk_spirv_effects_temporal_vert[];
extern const size_t		vk_spirv_effects_temporal_vert_size;
extern const uint32_t	vk_spirv_effects_temporal_clip_vert[];
extern const size_t		vk_spirv_effects_temporal_clip_vert_size;
extern const uint32_t	vk_spirv_temporal_composite_vert[];
extern const size_t		vk_spirv_temporal_composite_vert_size;
extern const uint32_t	vk_spirv_temporal_composite_frag[];
extern const size_t		vk_spirv_temporal_composite_frag_size;
extern const uint32_t	vk_spirv_hiz_prepare_comp[];
extern const size_t		vk_spirv_hiz_prepare_comp_size;
extern const uint32_t	vk_spirv_hiz_build_comp[];
extern const size_t		vk_spirv_hiz_build_comp_size;
extern const uint32_t	vk_spirv_hiz_tail_comp[];
extern const size_t		vk_spirv_hiz_tail_comp_size;
extern const uint32_t	vk_spirv_taa_resolve_comp[];
extern const size_t		vk_spirv_taa_resolve_comp_size;
extern const uint32_t	vk_spirv_ao_prefilter_comp[];
extern const size_t		vk_spirv_ao_prefilter_comp_size;
extern const uint32_t	vk_spirv_gtao_main_comp[];
extern const size_t		vk_spirv_gtao_main_comp_size;
extern const uint32_t	vk_spirv_ao_temporal_comp[];
extern const size_t		vk_spirv_ao_temporal_comp_size;
extern const uint32_t	vk_spirv_ao_denoise_comp[];
extern const size_t		vk_spirv_ao_denoise_comp_size;
extern const uint32_t	vk_spirv_ao_apply_frag[];
extern const size_t		vk_spirv_ao_apply_frag_size;
extern const uint32_t	vk_spirv_motion_blur_tiles_comp[];
extern const size_t		vk_spirv_motion_blur_tiles_comp_size;
extern const uint32_t	vk_spirv_motion_blur_neighbor_comp[];
extern const size_t		vk_spirv_motion_blur_neighbor_comp_size;
extern const uint32_t	vk_spirv_dof_focus_comp[];
extern const size_t		vk_spirv_dof_focus_comp_size;
extern const uint32_t	vk_spirv_dof_setup_comp[];
extern const size_t		vk_spirv_dof_setup_comp_size;
extern const uint32_t	vk_spirv_dof_neighbor_comp[];
extern const size_t		vk_spirv_dof_neighbor_comp_size;
extern const uint32_t	vk_spirv_dof_gather_comp[];
extern const size_t		vk_spirv_dof_gather_comp_size;
extern const uint32_t	vk_spirv_dof_apply_comp[];
extern const size_t		vk_spirv_dof_apply_comp_size;

/*
====================================================================

TEMPORAL PATH (vk_temporal.c)

r_temporal: the 3D views of a frame (a scene, vk_begin_scene) are drawn into
images of their own instead of the frame's color target: the color in
RGBA16F, the normals beside it and the motion vectors of what moves, the
depth image made sampled. When the scene ends (vk_end_scene: the 2D drawing
that follows it, another kind of view, the end of the frame) compute passes
make the depth pyramid, and the scene is drawn into the frame's color
target. The color and the pyramid of the last frame are kept: two of each,
the frames alternate. What the buffers don't hold (the camera's motion of
what doesn't move, the normals the draws didn't give) their readers work out
with shaders/temporal_common.glsl. Its readers: temporal anti-aliasing
(r_taa), ambient occlusion (r_ao), depth of field (r_dof) and motion blur
(r_motionBlur).

====================================================================
*/

#define VK_TEMPORAL_MAX_MIPS	16		// of the depth pyramid
#define VK_TEMPORAL_HISTORY		2		// images that alternate between frames
#define VK_TEMPORAL_MAX_PASSES	12		// dispatches of the pyramid after its levels 0 and 1
#define VK_TEMPORAL_TAIL_SIZE	128		// a level no larger: the rest in one dispatch (hiz_build.comp, TAIL); 256 was slower: one workgroup
#define VK_TEMPORAL_TAIL_IMAGES	8		// bindings of that dispatch: that level and up to 7 more
#define VK_TEMPORAL_MAX_BINDINGS	18	// of a pass's descriptor set (the ambient occlusion's)
#define VK_TEMPORAL_AO_LEVELS	5		// of the ambient occlusion's view distances (ao_common.glsl, AO_LEVELS)
#define VK_SKIN_RING			( VK_MAX_FRAMES + 1 )	// frame data of the skinned draws with the temporal path: see vk_alloc_skin_frame

// what the passes that read the scene's buffers need besides them
// (vkTemporal_t::readers): the depth image (readable) and the depth pyramid;
// without a reader that needs them the pyramid isn't made
#define VK_TEMPORAL_READ_DEPTH	1

// the images only some readers of the scene need, made while one of them is
// on and freed when none is (VK_TemporalUpdateFeatures, vkTemporal_t::groups)
#define VK_TEMPORAL_GROUP_TAA		1	// taa[2]: the anti-aliasing (r_taa)
#define VK_TEMPORAL_GROUP_PYRAMID	2	// hiz[2]: the anti-aliasing, r_temporalShow's views 1, 2, 4, 6, 7
#define VK_TEMPORAL_GROUP_AO		4	// the ambient occlusion's (r_ao)
#define VK_TEMPORAL_GROUP_PREVCOLOR	8	// color[1], the last scene's color: r_temporalShow 5
#define VK_TEMPORAL_GROUP_MOTIONBLUR	16	// mbVelocity, mbTile, mbNeighbor: the motion blur (r_motionBlur), r_temporalShow 11, 12
#define VK_TEMPORAL_GROUP_DOF		32	// dofFocus, dofHalf, dofTile, dofNeighbor, dofFar, dofNear, dofResult: the depth of field (r_dof), r_temporalShow 13
#define VK_TEMPORAL_GROUP_LAST		VK_TEMPORAL_GROUP_DOF

// a dispatch of the depth pyramid after the prepare pass (hiz_build.comp)
typedef struct {
	qboolean		tail;			// the TAIL variant: every level after src
	int				src;			// the level it reads
	int				count;			// the levels it makes after it (1 or 2 without tail)
} vkHizPass_t;

typedef struct {
	VkImage			image;
	VkDeviceMemory	memory;
	VkImageView		view;			// every level
} vkTemporalImage_t;

// the primary view for the passes after the scene and their readers:
// TemporalView of shaders/temporal_common.glsl
typedef struct {
	int				rect[4];		// x, y (from the top-left), width, height in framebuffer pixels
	float			proj[4];		// projection[0], [5], [8], [9] (unjittered)
	float			depth[4];		// projection[10], [14], the depth floor (vk_depth_floor), the far distance
	float			params[4];		// the jitter in NDC, 1 when the view has a last frame, VK_DEPTH_HACK_MAX
	float			reproject[16];	// the last frame's projection * the camera's motion (GL clip space)
} vkTemporalView_t;

typedef struct {
	// r_temporal latched on, without multisampling, on a device that has
	// what it needs (VK_TemporalCheckDevice); the rest is valid only then
	qboolean		enabled;
	qboolean		active;			// a scene is being drawn (between vk_begin_scene and vk_end_scene)
	qboolean		clearPending;	// the scene's first render pass clears its images (renderPassClear)
	uint64_t		sceneSerial;	// the frame of the last scene, 0 for none
	uint64_t		historySerial;	// the frame whose scene the other images hold, 0 for none
	int				current;		// the images of VK_TEMPORAL_HISTORY this frame's scene writes

	VkFormat		colorFormat, velocityFormat, normalFormat, hizFormat, taaFormat;
	// VK_TEMPORAL_GROUP_*: the groups of images made; their slots of the
	// static sets (the composite's, the ambient occlusion's apply pass) show
	// placeholder (1 x 1) while a group isn't
	int				groups;
	vkTemporalImage_t	placeholder;
	// latched for the frame's scene (vk_begin_scene, VK_TemporalUpdateFeatures)
	int				colorIndex;		// the scene's color image: current with the last scene's kept (VK_TEMPORAL_GROUP_PREVCOLOR), else 0
	int				showMode;		// r_temporalShow
	qboolean		aoOn;			// r_ao
	vkTemporalImage_t	color[VK_TEMPORAL_HISTORY];		// RGBA16F; color[1] only with VK_TEMPORAL_GROUP_PREVCOLOR
	vkTemporalImage_t	velocity;						// written by what moves (TEMPORAL_FLAG_MOTION)
	vkTemporalImage_t	normal;
	vkTemporalImage_t	hiz[VK_TEMPORAL_HISTORY];		// the pyramid, RG32F (min, max), level 0 at half size
	VkImageView		hizLevel[VK_TEMPORAL_HISTORY][VK_TEMPORAL_MAX_MIPS];	// a level each, for the storage writes
	int				hizWidth, hizHeight, hizLevels;
	uint64_t		hizSerial[VK_TEMPORAL_HISTORY];	// the frame each pyramid was made in, 0 for none
	// VK_TEMPORAL_READ_*: what the readers of the scene's buffers need, which
	// they set when they are created (none yet; r_taa and r_temporalShow's views add
	// theirs, VK_TemporalReaders)
	int				readers;
	vkHizPass_t		hizPasses[VK_TEMPORAL_MAX_PASSES];
	int				numHizPasses;
	VkImageView		depthView;		// the depth aspect of vk.depthImage, sampled
	// scene color, motion vectors, normals, depth: renderPassClear begins
	// a scene (the color and the normals cleared), renderPass continues it
	VkRenderPass	renderPass, renderPassClear;
	VkFramebuffer	framebuffers[VK_TEMPORAL_HISTORY];	// with color[i]
	VkSampler		sampler;		// nearest, clamped (the shaders fetch texels)
	VkSampler		linearSampler;	// bilinear, clamped: the anti-aliasing's history
	vkBuffer_t		zeroBuffer;		// (0, 0, 0): the normals of the draws that have none

	// the scene's pipelines (VK_PIPE_TEMPORAL): set 2 adds the last frame's
	// frame data of the skinned draws, the push constants vkTemporalPush_t
	VkDescriptorSetLayout graphicsSetLayout;
	VkPipelineLayout	pipelineLayout;
	VkShaderModule	vertexShader, vertexShaderClip;
	VkShaderModule	skinVertexShader, skinVertexShaderClip;
	VkShaderModule	effectsVertexShader, effectsVertexShaderClip;
	VkShaderModule	fragmentShader;
	VkDescriptorSet	graphicsSets[VK_MAX_FRAMES];

	// the passes after the scene: the prepare pass's set is written in each
	// frame (the depth image can be replaced), the others once
	VkDescriptorSetLayout prepareSetLayout, hizSetLayout, compositeSetLayout;
	VkPipelineLayout	prepareLayout, hizLayout, compositeLayout;
	VkShaderModule	prepareShader, hizShader, hizTailShader, compositeVertexShader, compositeFragmentShader;
	VkPipeline		preparePipeline, hizPipeline, hizTailPipeline, compositePipeline;
	VkPipeline		compositeBlurPipeline;	// the same with the motion blur's gather (MOTION_BLUR)
	VkDescriptorPool	pool;
	VkDescriptorSet	prepareSets[VK_MAX_FRAMES];
	VkDescriptorSet	hizSets[VK_TEMPORAL_HISTORY][VK_TEMPORAL_MAX_PASSES];
	VkDescriptorSet	compositeSets[VK_TEMPORAL_HISTORY];
	VkDescriptorSet	compositeTaaSets[VK_TEMPORAL_HISTORY];	// the same, showing taa[current]
	VkDescriptorSet	compositeDofSets[VK_TEMPORAL_HISTORY];	// the same, showing dofResult

	// temporal anti-aliasing (r_taa, taa_resolve.comp): its result, this
	// frame's and the last frame's (the history), alternating with current;
	// GENERAL between frames
	qboolean		taaOn;			// r_taa for this frame's scene (vk_begin_scene): it is jittered
	qboolean		taaRan;			// the resolve wrote taa[current]: the composite shows it
	vkTemporalImage_t	taa[VK_TEMPORAL_HISTORY];	// RGBA16F (taaFormat)
	uint64_t		taaSerial[VK_TEMPORAL_HISTORY];	// the frame each was written in, 0 for none
	VkDescriptorSetLayout taaSetLayout;
	VkPipelineLayout	taaLayout;
	VkShaderModule	taaShader;
	VkPipeline		taaPipeline;
	VkDescriptorSet	taaSets[VK_MAX_FRAMES];	// written in each frame (the depth image's view can change)

	// ambient occlusion (r_ao, VK_TemporalAo: ao_*.comp, gtao_main.comp):
	// computed and blended into the scene between the primary view's
	// opaque surfaces and its blended ones; its images are GENERAL always
	qboolean		aoRan;			// this frame's scene has it
	vkTemporalImage_t	aoDepth[VK_TEMPORAL_HISTORY];	// R32F view distances (the weapon's negative), VK_TEMPORAL_AO_LEVELS levels; the other one the last frame's
	VkImageView		aoDepthLevel[VK_TEMPORAL_HISTORY][VK_TEMPORAL_AO_LEVELS];
	uint64_t		aoDepthSerial[VK_TEMPORAL_HISTORY];	// the frame each was made in, 0 for none
	vkTemporalImage_t	aoRaw;			// R8: one frame's visibility
	vkTemporalImage_t	aoHistory[VK_TEMPORAL_HISTORY];	// RG16F: visibility blended over frames, their count
	uint64_t		aoSerial[VK_TEMPORAL_HISTORY];
	vkTemporalImage_t	aoBlur;			// RG16F: the first blur of two
	vkTemporalImage_t	aoResult;		// RGBA8: what the apply pass blends (the fog's part, the factor)
	VkDescriptorSetLayout aoSetLayout, aoApplySetLayout;
	VkPipelineLayout	aoLayout, aoApplyLayout;
	VkShaderModule	aoPrefilterShader, aoMainShader, aoTemporalShader, aoDenoiseShader, aoApplyShader;
	VkPipeline		aoPrefilterPipeline, aoTemporalPipeline, aoDenoisePipeline, aoApplyPipeline;
	VkPipeline		aoMainPipelines[4];	// r_aoQuality: slices and steps as specialization constants
	VkDescriptorSet	aoSets[VK_MAX_FRAMES];	// written in each frame
	VkDescriptorSet	aoApplySet;

	// motion blur (r_motionBlur, VK_TemporalMotionBlur: motion_blur.comp,
	// the gather in temporal_composite.frag): each pixel's blur, its half
	// vector in pixels and its view distance, the longest half vector of each
	// tile of mbTileSize x mbTileSize pixels and of the 3 x 3 tiles around it;
	// its images are GENERAL always
	qboolean		mbOn;			// for this frame's scene (VK_TemporalUpdateFeatures): its passes run (r_motionBlur, r_temporalShow 11, 12)
	qboolean		mbBlur;			// and the composite blurs (r_motionBlur)
	qboolean		mbRan;			// the passes ran in this scene
	float			frameMsec;		// the primary view's game time since its last frame, smoothed (vkSceneView_t)
	int				mbTileSize;		// the longest half streak in pixels: max( 16, height / 40 )
	int				mbTilesX, mbTilesY;
	vkTemporalImage_t	mbVelocity;		// RGBA16F: the half vector (xy), the view distance (z; the weapon's scaled down, in front of all)
	vkTemporalImage_t	mbTile;			// RG16F, a texel a tile
	vkTemporalImage_t	mbNeighbor;		// RG16F, a texel a tile
	VkDescriptorSetLayout mbSetLayout;
	VkPipelineLayout	mbLayout;
	VkShaderModule	mbTilesShader, mbNeighborShader;
	VkPipeline		mbTilesPipeline, mbNeighborPipeline;
	VkDescriptorSet	mbSets[VK_MAX_FRAMES];	// written in each frame (the depth image's view can change)

	// depth of field (r_dof, VK_TemporalDof: dof.comp): the focus, the half
	// resolution's color and blur, the largest blur of each tile of
	// dofTileSize x dofTileSize pixels and of the 3 x 3 tiles around it, the
	// gathered background and foreground, the result the composite shows;
	// its images are GENERAL always
	qboolean		dofOn;			// for this frame's scene (VK_TemporalUpdateFeatures): its passes run (r_dof, r_temporalShow 13)
	qboolean		dofShow;		// r_temporalShow 13: the result shows the blur instead
	qboolean		dofRan;			// the passes ran in this scene: the composite shows dofResult
	int				dofTileSize;	// the largest blur's radius in pixels, even: max( 16, height / 20 )
	int				dofTilesX, dofTilesY;
	int				dofHalfWidth, dofHalfHeight;
	vkTemporalImage_t	dofFocus;		// R32F, 1 x 1: 1 / the focus distance, eased over frames; 0: none yet
	vkTemporalImage_t	dofHalf;		// RGBA16F, half resolution: color in linear light, the blur
	vkTemporalImage_t	dofTile;		// RG16F, a texel a tile
	vkTemporalImage_t	dofNeighbor;	// RG16F, a texel a tile
	vkTemporalImage_t	dofFar;			// RGBA16F, half resolution
	vkTemporalImage_t	dofNear;		// RGBA16F, half resolution
	vkTemporalImage_t	dofResult;		// RGBA16F (gamma-encoded, as the scene)
	VkDescriptorSetLayout dofSetLayout;
	VkPipelineLayout	dofLayout;
	VkShaderModule	dofFocusShader, dofSetupShader, dofNeighborShader, dofGatherShader, dofApplyShader;
	VkPipeline		dofFocusPipeline, dofSetupPipeline, dofNeighborPipeline, dofGatherPipeline, dofApplyPipeline;
	VkDescriptorSet	dofSets[VK_MAX_FRAMES];	// written in each frame (the depth image's and the color's views change)

	// the frame data of the skinned draws: a ring of VK_SKIN_RING buffers
	// instead of one per frame slot (the frame slots' and one more), so the
	// last frame's is still there while the next frame writes its own
	// (skinned.vert reads the last frame's bones)
	vkBuffer_t		*skinRing[VK_SKIN_RING];
	vkBuffer_t		skinExtra;
	int				skinSlot;		// the current frame's

	// the scene's views (vk_begin_scene)
	qboolean		viewHistory;	// the view has a last frame: prevProjection and cameraDelta are valid
	float			prevProjection[16];	// the last frame's projection, unjittered
	float			cameraDelta[16];	// the last frame's view matrix * the inverse of this view's
	qboolean		jitter;			// the scene's perspective projections are jittered
	float			jitterNdc[2];
	float			jitterPixels[2];	// the same in pixels (x right, y down), for the resolve
	int				rect[4];		// the union of the views, in framebuffer pixels from the top-left (x, y, width, height)
	// the primary view (the first that is neither a portal nor the sky
	// portal), for the passes after the scene
	qboolean		primarySet;
	float			projection[16];	// its projection, unjittered
	vkTemporalView_t	view;
	// the explicit last frame's modelview of the draws (vk_set_prev_modelview)
	qboolean		prevModelviewSet;
	float			prevModelview[16];
	float			prevModelviewFor[16];	// the modelview it was given with
	qboolean		objectMotion;	// it applies: the draws move (VK_PIPE_MOTION)
} vkTemporal_t;

extern vkTemporal_t	vkt;

void		VK_TemporalRegisterCvars( void );
void		VK_TemporalCheckDevice( void );
void		VK_TemporalCreateTargets( void );
void		VK_TemporalDestroyTargets( void );
void		VK_TemporalDepthChanged( void );
void		VK_TemporalUpdateFeatures( void );
void		VK_TemporalInitPipelines( void );
void		VK_TemporalShutdownPipelines( void );
void		VK_TemporalBeginFrame( vkFrame_t *frame, VkBuffer skinData );
void		VK_TemporalResolve( VkCommandBuffer cmd );
void		VK_TemporalComposite( VkCommandBuffer cmd );
qboolean	VK_TemporalAoWanted( void );
void		VK_TemporalAo( VkCommandBuffer cmd, const float *fog );
void		VK_TemporalAoApply( VkCommandBuffer cmd );
qboolean	VK_TemporalWritesBuffers( uint64_t bits );

// cvars
extern cvar_t	*r_vkValidation;
extern cvar_t	*r_vkFallbacks;
extern cvar_t	*r_vkSubmitThread;
extern cvar_t	*r_fastSync;
extern cvar_t	*r_ext_multisample;
extern cvar_t	*r_temporal;
extern cvar_t	*r_temporalJitter;
extern cvar_t	*r_temporalJitterSamples;
extern cvar_t	*r_temporalShow;
extern cvar_t	*r_temporalShowMip;
extern cvar_t	*r_temporalShowScale;
extern cvar_t	*r_taa;
extern cvar_t	*r_taaFeedback;
extern cvar_t	*r_taaSharpen;
extern cvar_t	*r_saturation;
extern cvar_t	*r_ao;
extern cvar_t	*r_motionBlur;
extern cvar_t	*r_dof;
