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

// vk_image.c -- textures, samplers and texture uploads
//
// image_t::texnum is a handle into a table of slots; each slot points to the
// current storage of the image (VkImage, view, memory and a descriptor set
// combining the view with its sampler). Replacing or destroying a storage
// defers the release of its Vulkan objects until the frame that may still
// use them has completed, so draws recorded before keep the old storage.
//
// Uploads are copied to a per-frame staging buffer and recorded in one of
// two command buffers of the frame:
//  - the upload command buffer, submitted before the main one, when the
//    storage hasn't been used by a draw recorded in the main command buffer
//    since its last submission (new textures, level loading, cinematics and
//    dynamic lightmaps updated once per frame)
//  - the main command buffer otherwise, so draws recorded before the upload
//    see the old contents; this ends the render pass
// Between command buffers every storage is in SHADER_READ_ONLY_OPTIMAL. A
// storage stays in TRANSFER_DST_OPTIMAL while uploads are recorded for it and
// goes back to SHADER_READ_ONLY_OPTIMAL before the next render pass of the
// main command buffer, or at the end of the upload command buffer.
//
// Memory: the images are placed in blocks of device memory (see MEMORY
// below) rather than getting one allocation each.

#include "vk_local.h"

#define VK_SETS_PER_POOL		1024
#define VK_MAX_DESCRIPTOR_POOLS	64
#define VK_SAMPLER_NOMIP		6		// sampler index of textures with one mip level

#define VK_MEMORY_BLOCK_SIZE	( 64 * 1024 * 1024 )
#define VK_MAX_MEMORY_BLOCKS	256

typedef struct {
	VkDeviceSize	offset, size;
} vkMemoryRange_t;

// a block of device memory that texture images are placed in
typedef struct {
	VkDeviceMemory	memory;			// VK_NULL_HANDLE: unused entry
	uint32_t		memoryType;
	VkDeviceSize	size, used;
	vkMemoryRange_t	*free;			// free ranges, sorted by offset, never adjacent
	int				numFree, maxFree;
} vkMemoryBlock_t;

typedef struct {
	vkTexture_t	**textures;
	int			num, max;
} vkTextureList_t;

static vkTextureSlot_t	vk_slots[VK_MAX_TEXTURES];
static unsigned int		vk_slotGeneration[VK_MAX_TEXTURES];
static int				vk_nextSlot;

static vkTexture_t		*vk_defaultTexture;

static VkDescriptorPool	vk_descriptorPools[VK_MAX_DESCRIPTOR_POOLS];
static int				vk_numDescriptorPools;

// [TEXFILTER_* or VK_SAMPLER_NOMIP][wrapX is clamp][wrapY is clamp]
static VkSampler		vk_samplers[VK_SAMPLER_NOMIP + 1][2][2];
static texFilter_t		vk_textureFilter = TEXFILTER_LINEAR_MIPMAP_NEAREST;

// storages in TRANSFER_DST_OPTIMAL in the upload and main command buffers
static vkTextureList_t	vk_pendingUpload;
static vkTextureList_t	vk_pendingMain;

static vkMemoryBlock_t	vk_memoryBlocks[VK_MAX_MEMORY_BLOCKS];

/*
====================================================================

HELPERS

====================================================================
*/

static void VK_ListAdd( vkTextureList_t *list, vkTexture_t *texture ) {
	if ( list->num == list->max ) {
		vkTexture_t **textures;

		list->max = list->max ? list->max * 2 : 256;
		textures = VK_Alloc( sizeof( *textures ) * list->max );
		if ( list->num ) {
			Com_Memcpy( textures, list->textures, sizeof( *textures ) * list->num );
		}
		if ( list->textures ) {
			VK_Free( list->textures );
		}
		list->textures = textures;
	}
	list->textures[list->num++] = texture;
}

static void VK_ListRemove( vkTextureList_t *list, vkTexture_t *texture ) {
	int i;

	for ( i = 0; i < list->num; i++ ) {
		if ( list->textures[i] == texture ) {
			list->textures[i] = list->textures[--list->num];
			return;
		}
	}
}

static void VK_ListFree( vkTextureList_t *list ) {
	if ( list->textures ) {
		VK_Free( list->textures );
	}
	Com_Memset( list, 0, sizeof( *list ) );
}

static VkFormat VK_TextureFormat( texFormat_t format, qboolean *rgb, qboolean *compressed ) {
	*rgb = qfalse;
	*compressed = qfalse;

	switch ( format ) {
	case TEXFMT_RGB8:
	case TEXFMT_RGB5:
		*rgb = qtrue;
		return VK_FORMAT_R8G8B8A8_UNORM;
	case TEXFMT_BC1_RGB:
		*rgb = qtrue;
		*compressed = qtrue;
		return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
	case TEXFMT_BC1_RGBA:
		*compressed = qtrue;
		return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	case TEXFMT_BC2:
		*compressed = qtrue;
		return VK_FORMAT_BC2_UNORM_BLOCK;
	case TEXFMT_BC3:
		*compressed = qtrue;
		return VK_FORMAT_BC3_UNORM_BLOCK;
	case TEXFMT_RGBA8:
	case TEXFMT_RGBA4:
	default:
		return VK_FORMAT_R8G8B8A8_UNORM;
	}
}

static int VK_BlockSize( VkFormat format ) {
	switch ( format ) {
	case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
		return 8;
	case VK_FORMAT_BC2_UNORM_BLOCK:
	case VK_FORMAT_BC3_UNORM_BLOCK:
		return 16;
	default:
		return 4;
	}
}

/*
====================================================================

MEMORY

Texture images are sub-allocated from blocks of VK_MEMORY_BLOCK_SIZE bytes
of device memory, first fit, instead of one vkAllocateMemory each: every
allocation is a kernel object that takes time to create and that the driver
may have to reference in each submission, and some drivers allow only 4096
of them (maxMemoryAllocationCount). An image larger than a quarter of a
block gets memory of its own. Only images with optimal tiling go into the
blocks, so bufferImageGranularity doesn't apply. Ranges are released through
the deferred destruction lists like the images, once no frame in flight can
use them; a block that becomes empty is freed unless it is the last one of
its memory type.

====================================================================
*/

static void VK_InsertFreeRange( vkMemoryBlock_t *block, int index, VkDeviceSize offset, VkDeviceSize size ) {
	if ( block->numFree == block->maxFree ) {
		vkMemoryRange_t *ranges;

		block->maxFree = block->maxFree ? block->maxFree * 2 : 64;
		ranges = VK_Alloc( sizeof( *ranges ) * block->maxFree );
		if ( block->numFree ) {
			Com_Memcpy( ranges, block->free, sizeof( *ranges ) * block->numFree );
		}
		if ( block->free ) {
			VK_Free( block->free );
		}
		block->free = ranges;
	}

	memmove( &block->free[index + 1], &block->free[index], sizeof( *block->free ) * ( block->numFree - index ) );
	block->free[index].offset = offset;
	block->free[index].size = size;
	block->numFree++;
}

static void VK_RemoveFreeRange( vkMemoryBlock_t *block, int index ) {
	memmove( &block->free[index], &block->free[index + 1], sizeof( *block->free ) * ( block->numFree - index - 1 ) );
	block->numFree--;
}

static qboolean VK_BlockAlloc( vkMemoryBlock_t *block, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize *offset ) {
	int i;

	for ( i = 0; i < block->numFree; i++ ) {
		vkMemoryRange_t *range = &block->free[i];
		VkDeviceSize start = PAD( range->offset, alignment );
		VkDeviceSize end = range->offset + range->size;

		if ( start + size > end ) {
			continue;
		}

		// what is left before the start (alignment) and after the end stays free
		if ( start > range->offset ) {
			range->size = start - range->offset;
			if ( start + size < end ) {
				VK_InsertFreeRange( block, i + 1, start + size, end - ( start + size ) );
			}
		} else if ( start + size < end ) {
			range->offset = start + size;
			range->size = end - range->offset;
		} else {
			VK_RemoveFreeRange( block, i );
		}

		block->used += size;
		*offset = start;
		return qtrue;
	}

	return qfalse;
}

static void VK_AllocateImageMemory( vkTexture_t *texture ) {
	VkMemoryRequirements memReqs;
	vkMemoryBlock_t *block;
	uint32_t memoryType;
	int i;

	qvkGetImageMemoryRequirements( vk.device, texture->image, &memReqs );
	memoryType = VK_FindMemoryType( memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 );
	texture->memorySize = memReqs.size;

	if ( memReqs.size <= VK_MEMORY_BLOCK_SIZE / 4 ) {
		for ( i = 0; i < VK_MAX_MEMORY_BLOCKS; i++ ) {
			block = &vk_memoryBlocks[i];
			if ( block->memory && block->memoryType == memoryType
				&& VK_BlockAlloc( block, memReqs.size, memReqs.alignment, &texture->memoryOffset ) ) {
				break;
			}
		}

		if ( i == VK_MAX_MEMORY_BLOCKS ) {
			// a new block
			for ( i = 0; i < VK_MAX_MEMORY_BLOCKS && vk_memoryBlocks[i].memory; i++ ) {
			}
			if ( i < VK_MAX_MEMORY_BLOCKS ) {
				block = &vk_memoryBlocks[i];
				block->memory = VK_AllocateMemory( VK_MEMORY_BLOCK_SIZE, memoryType );
				block->memoryType = memoryType;
				block->size = VK_MEMORY_BLOCK_SIZE;
				block->used = 0;
				block->numFree = 0;
				VK_InsertFreeRange( block, 0, 0, VK_MEMORY_BLOCK_SIZE );
				VK_BlockAlloc( block, memReqs.size, memReqs.alignment, &texture->memoryOffset );
			}
		}

		if ( i < VK_MAX_MEMORY_BLOCKS ) {
			texture->memoryBlock = i;
			texture->memory = vk_memoryBlocks[i].memory;
			VK_CHECK( qvkBindImageMemory( vk.device, texture->image, texture->memory, texture->memoryOffset ) );
			return;
		}
	}

	// too large for a block, or no block left
	texture->memoryBlock = -1;
	texture->memoryOffset = 0;
	texture->memory = VK_AllocateMemory( memReqs.size, memoryType );
	VK_CHECK( qvkBindImageMemory( vk.device, texture->image, texture->memory, 0 ) );
}

/*
===============
VK_FreeMemoryRange

Called from the deferred destruction lists
===============
*/
void VK_FreeMemoryRange( int index, VkDeviceSize offset, VkDeviceSize size ) {
	vkMemoryBlock_t *block;
	qboolean mergePrev, mergeNext;
	int i;

	if ( index < 0 || index >= VK_MAX_MEMORY_BLOCKS || !vk_memoryBlocks[index].memory ) {
		return;
	}
	block = &vk_memoryBlocks[index];

	// the first free range after the one released
	for ( i = 0; i < block->numFree && block->free[i].offset < offset; i++ ) {
	}
	mergePrev = i > 0 && block->free[i - 1].offset + block->free[i - 1].size == offset;
	mergeNext = i < block->numFree && offset + size == block->free[i].offset;

	if ( mergePrev && mergeNext ) {
		block->free[i - 1].size += size + block->free[i].size;
		VK_RemoveFreeRange( block, i );
	} else if ( mergePrev ) {
		block->free[i - 1].size += size;
	} else if ( mergeNext ) {
		block->free[i].offset = offset;
		block->free[i].size += size;
	} else {
		VK_InsertFreeRange( block, i, offset, size );
	}
	block->used -= size;

	if ( block->used ) {
		return;
	}

	// keep one empty block per memory type for the next textures
	for ( i = 0; i < VK_MAX_MEMORY_BLOCKS; i++ ) {
		if ( i != index && vk_memoryBlocks[i].memory && vk_memoryBlocks[i].memoryType == block->memoryType ) {
			break;
		}
	}
	if ( i < VK_MAX_MEMORY_BLOCKS ) {
		VK_FreeMemory( block->memory );
		block->memory = VK_NULL_HANDLE;
		block->numFree = 0;
	}
}

void VK_MemoryBlockInfo( int *numBlocks, VkDeviceSize *size, VkDeviceSize *used ) {
	int i;

	*numBlocks = 0;
	*size = *used = 0;
	for ( i = 0; i < VK_MAX_MEMORY_BLOCKS; i++ ) {
		if ( vk_memoryBlocks[i].memory ) {
			( *numBlocks )++;
			*size += vk_memoryBlocks[i].size;
			*used += vk_memoryBlocks[i].used;
		}
	}
}

static void VK_ShutdownMemory( void ) {
	int i;

	for ( i = 0; i < VK_MAX_MEMORY_BLOCKS; i++ ) {
		vkMemoryBlock_t *block = &vk_memoryBlocks[i];

		if ( block->memory ) {
			VK_FreeMemory( block->memory );
		}
		if ( block->free ) {
			VK_Free( block->free );
		}
	}
	Com_Memset( vk_memoryBlocks, 0, sizeof( vk_memoryBlocks ) );
}

/*
====================================================================

SAMPLERS AND DESCRIPTORS

====================================================================
*/

static VkSampler VK_GetSampler( int filter, int wrapX, int wrapY ) {
	VkSamplerCreateInfo info;
	VkSampler *sampler;
	int clampX, clampY;

	clampX = wrapX == WRAP_CLAMP;
	clampY = wrapY == WRAP_CLAMP;
	sampler = &vk_samplers[filter][clampX][clampY];
	if ( *sampler ) {
		return *sampler;
	}

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.addressModeU = clampX ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.addressModeV = clampY ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.minLod = 0.0f;
	info.maxLod = VK_LOD_CLAMP_NONE;
	info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

	// the base filter is the same for minification and magnification in
	// every GL mode the renderer uses
	switch ( filter ) {
	case TEXFILTER_NEAREST:
		info.minFilter = info.magFilter = VK_FILTER_NEAREST;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.maxLod = 0.0f;
		break;
	case TEXFILTER_NEAREST_MIPMAP_NEAREST:
		info.minFilter = info.magFilter = VK_FILTER_NEAREST;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		break;
	case TEXFILTER_LINEAR_MIPMAP_NEAREST:
		info.minFilter = info.magFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		break;
	case TEXFILTER_NEAREST_MIPMAP_LINEAR:
		info.minFilter = info.magFilter = VK_FILTER_NEAREST;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		break;
	case TEXFILTER_LINEAR_MIPMAP_LINEAR:
		info.minFilter = info.magFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		break;
	case TEXFILTER_LINEAR:
	case VK_SAMPLER_NOMIP:
	default:
		info.minFilter = info.magFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.maxLod = 0.0f;
		break;
	}

	// anisotropy for mipmapped textures, as GL1 set it
	if ( filter != VK_SAMPLER_NOMIP && textureFilterAnisotropic && vk.enabledFeatures.samplerAnisotropy ) {
		float level = Com_Clamp( 1, maxAnisotropy, r_ext_max_anisotropy->integer );

		if ( level > 1.0f ) {
			info.anisotropyEnable = VK_TRUE;
			info.maxAnisotropy = level;
		}
	}

	VK_CHECK( qvkCreateSampler( vk.device, &info, NULL, sampler ) );
	return *sampler;
}

static void VK_AllocateDescriptorSet( vkTexture_t *texture ) {
	VkDescriptorSetAllocateInfo allocInfo;
	VkDescriptorImageInfo imageInfo;
	VkWriteDescriptorSet write;
	VkResult result;
	int i;

	Com_Memset( &allocInfo, 0, sizeof( allocInfo ) );
	allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &vk.textureSetLayout;

	// newest pool first, it's the one most likely to have room
	result = VK_ERROR_OUT_OF_POOL_MEMORY;
	for ( i = vk_numDescriptorPools - 1; i >= 0; i-- ) {
		allocInfo.descriptorPool = vk_descriptorPools[i];
		result = qvkAllocateDescriptorSets( vk.device, &allocInfo, &texture->descriptorSet );
		if ( result == VK_SUCCESS ) {
			break;
		}
		if ( result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL ) {
			VK_CHECK( result );
		}
	}

	if ( result != VK_SUCCESS ) {
		VkDescriptorPoolCreateInfo poolInfo;
		VkDescriptorPoolSize poolSize;

		if ( vk_numDescriptorPools == VK_MAX_DESCRIPTOR_POOLS ) {
			ri.Error( ERR_FATAL, "Vulkan: out of texture descriptor pools" );
		}

		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = VK_SETS_PER_POOL;

		Com_Memset( &poolInfo, 0, sizeof( poolInfo ) );
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets = VK_SETS_PER_POOL;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		VK_CHECK( qvkCreateDescriptorPool( vk.device, &poolInfo, NULL, &vk_descriptorPools[vk_numDescriptorPools] ) );

		allocInfo.descriptorPool = vk_descriptorPools[vk_numDescriptorPools++];
		VK_CHECK( qvkAllocateDescriptorSets( vk.device, &allocInfo, &texture->descriptorSet ) );
	}
	texture->descriptorPool = allocInfo.descriptorPool;

	imageInfo.sampler = VK_GetSampler( texture->mipLevels > 1 ? vk_textureFilter : VK_SAMPLER_NOMIP, texture->wrapX, texture->wrapY );
	imageInfo.imageView = texture->view;
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	Com_Memset( &write, 0, sizeof( write ) );
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = texture->descriptorSet;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &imageInfo;
	qvkUpdateDescriptorSets( vk.device, 1, &write, 0, NULL );
}

/*
====================================================================

STORAGE

====================================================================
*/

static void VK_TransitionToTransfer( VkCommandBuffer cmd, vkTexture_t *texture, VkImageLayout oldLayout ) {
	VK_ImageBarrier( cmd, texture->image, VK_IMAGE_ASPECT_COLOR_BIT,
		oldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
}

static vkTexture_t *VK_CreateTextureStorage( int width, int height, int mipLevels, texFormat_t texFormat, int wrapX, int wrapY ) {
	VkImageCreateInfo imageInfo;
	VkImageViewCreateInfo viewInfo;
	vkTexture_t *texture;
	uint64_t start;
	int maxLevels;

	start = VK_Microseconds();

	texture = VK_Alloc( sizeof( *texture ) );
	Com_Memset( texture, 0, sizeof( *texture ) );

	width = Com_Clamp( 1, vk.deviceProperties.limits.maxImageDimension2D, width );
	height = Com_Clamp( 1, vk.deviceProperties.limits.maxImageDimension2D, height );
	for ( maxLevels = 1; ( width >> maxLevels ) || ( height >> maxLevels ); maxLevels++ ) {
	}

	texture->format = VK_TextureFormat( texFormat, &texture->rgb, &texture->compressed );
	texture->width = width;
	texture->height = height;
	texture->mipLevels = Com_Clamp( 1, maxLevels, mipLevels );
	texture->wrapX = wrapX;
	texture->wrapY = wrapY;

	Com_Memset( &imageInfo, 0, sizeof( imageInfo ) );
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = texture->format;
	imageInfo.extent.width = width;
	imageInfo.extent.height = height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = texture->mipLevels;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK( qvkCreateImage( vk.device, &imageInfo, NULL, &texture->image ) );
	VK_AllocateImageMemory( texture );

	Com_Memset( &viewInfo, 0, sizeof( viewInfo ) );
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = texture->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = texture->format;
	viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	// GL ignores the alpha of textures with an RGB internal format; without
	// imageViewFormatSwizzle (portability subset) the fragment shader sets
	// it (VK_PIPE_TEX*_RGB)
	viewInfo.components.a = ( texture->rgb && vk.viewSwizzle ) ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = texture->mipLevels;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &viewInfo, NULL, &texture->view ) );

	VK_AllocateDescriptorSet( texture );

	// a valid layout before any upload; the contents are undefined, as
	// they were in GL until the first glTexSubImage2D
	VK_TransitionToTransfer( VK_GetUploadCommandBuffer(), texture, VK_IMAGE_LAYOUT_UNDEFINED );
	texture->uploadPending = qtrue;
	VK_ListAdd( &vk_pendingUpload, texture );

	vk.stats.textures++;
	vk.stats.textureUs += VK_Microseconds() - start;

	return texture;
}

static void VK_DestroyTextureStorage( vkTexture_t *texture ) {
	if ( texture->uploadPending ) {
		// draws recorded in the main command buffer may sample it after the
		// upload command buffer: leave it in the layout they expect
		VK_ImageBarrier( VK_GetUploadCommandBuffer(), texture->image, VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
		VK_ListRemove( &vk_pendingUpload, texture );
	}
	if ( texture->mainPending ) {
		// no draw can use it after the upload that put it in this layout
		VK_ListRemove( &vk_pendingMain, texture );
	}

	VK_DeferDestroy( VK_DESTROY_DESCRIPTOR_SET, (uint64_t)texture->descriptorSet, (uint64_t)texture->descriptorPool );
	VK_DeferDestroy( VK_DESTROY_IMAGE_VIEW, (uint64_t)texture->view, 0 );
	VK_DeferDestroy( VK_DESTROY_IMAGE, (uint64_t)texture->image, 0 );
	if ( texture->memoryBlock >= 0 ) {
		VK_DeferDestroyRange( texture->memoryBlock, texture->memoryOffset, texture->memorySize );
	} else {
		VK_DeferDestroy( VK_DESTROY_MEMORY, (uint64_t)texture->memory, 0 );
	}

	VK_Free( texture );
}

/*
===============
VK_RecordUpload

Records the copy of one region from a staging buffer into the texture
===============
*/
static void VK_RecordUpload( vkTexture_t *texture, int mipLevel, int x, int y, int width, int height, VkBuffer buffer, VkDeviceSize offset ) {
	VkBufferImageCopy region;
	VkCommandBuffer cmd;
	uint32_t mipBit;

	mipBit = 1u << mipLevel;

	if ( texture->lastUseSegment == vk.segment ) {
		// draws recorded since the last submission use the old contents
		VK_EndRenderPass();
		cmd = vk.frame->mainCmd;
		vk.stats.mainUploads++;

		if ( !texture->mainPending ) {
			VK_TransitionToTransfer( cmd, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
			texture->mainPending = qtrue;
			texture->mainMips = 0;
			VK_ListAdd( &vk_pendingMain, texture );
		} else if ( texture->mainMips & mipBit ) {
			// two copies to the same level
			VK_ImageBarrier( cmd, texture->image, VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
		}
		texture->mainMips |= mipBit;
	} else {
		cmd = VK_GetUploadCommandBuffer();

		if ( !texture->uploadPending ) {
			VK_TransitionToTransfer( cmd, texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL );
			texture->uploadPending = qtrue;
			texture->uploadMips = 0;
			VK_ListAdd( &vk_pendingUpload, texture );
		} else if ( texture->uploadMips & mipBit ) {
			VK_ImageBarrier( cmd, texture->image, VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
		}
		texture->uploadMips |= mipBit;
	}

	Com_Memset( &region, 0, sizeof( region ) );
	region.bufferOffset = offset;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = mipLevel;
	region.imageSubresource.layerCount = 1;
	region.imageOffset.x = x;
	region.imageOffset.y = y;
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = 1;
	qvkCmdCopyBufferToImage( cmd, buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
}

/*
===============
VK_FinishUploads

Puts the textures uploaded in the upload or the main command buffer back
in SHADER_READ_ONLY_OPTIMAL; called outside of a render pass
===============
*/
void VK_FinishUploads( VkCommandBuffer cmd, qboolean upload ) {
	VkImageMemoryBarrier barriers[64];
	vkTextureList_t *list;
	int i, n;

	list = upload ? &vk_pendingUpload : &vk_pendingMain;

	for ( i = 0; i < list->num; ) {
		for ( n = 0; n < (int)ARRAY_LEN( barriers ) && i < list->num; n++, i++ ) {
			vkTexture_t *texture = list->textures[i];

			Com_Memset( &barriers[n], 0, sizeof( barriers[n] ) );
			barriers[n].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barriers[n].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barriers[n].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			barriers[n].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barriers[n].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barriers[n].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[n].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[n].image = texture->image;
			barriers[n].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barriers[n].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
			barriers[n].subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

			if ( upload ) {
				texture->uploadPending = qfalse;
			} else {
				texture->mainPending = qfalse;
			}
		}

		qvkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, NULL, 0, NULL, n, barriers );
	}

	list->num = 0;
}

/*
===============
VK_StagingAlloc

Returns host memory for size bytes of upload data and the buffer range it
maps to. Submits the frame so far and waits for it when the staging buffer
of the frame is full.
===============
*/
void *VK_StagingAlloc( VkDeviceSize size, VkDeviceSize alignment, VkBuffer *buffer, VkDeviceSize *offset ) {
	vkFrame_t *frame;
	VkDeviceSize start;

	VK_BeginFrameIfNeeded();
	frame = vk.frame;

	if ( size > frame->stagingBuffer.size ) {
		vkBuffer_t temp;

		// too big for any staging buffer: a buffer of its own, released
		// with the frame
		VK_CreateBuffer( &temp, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0 );
		VK_DeferDestroy( VK_DESTROY_BUFFER, (uint64_t)temp.buffer, 0 );
		VK_DeferDestroy( VK_DESTROY_MEMORY, (uint64_t)temp.memory, 0 );
		vk.stats.stagingBytes += size;
		*buffer = temp.buffer;
		*offset = 0;
		return temp.mapped;
	}

	start = PAD( frame->stagingOffset, alignment );
	if ( start + size > frame->stagingBuffer.size ) {
		VK_FlushAndWait();
		frame = vk.frame;
		start = 0;
	}

	frame->stagingOffset = start + size;
	vk.stats.stagingBytes += size;
	*buffer = frame->stagingBuffer.buffer;
	*offset = start;
	return frame->stagingBuffer.mapped + start;
}

/*
====================================================================

TEXTURE TABLE

====================================================================
*/

static int VK_SlotForHandle( unsigned int handle ) {
	int slot;

	if ( !handle ) {
		return -1;
	}
	slot = ( handle - 1 ) % VK_MAX_TEXTURES;
	if ( vk_slots[slot].handle != handle ) {
		return -1;
	}
	return slot;
}

static int VK_AllocSlot( void ) {
	int i, slot;

	for ( i = 0; i < VK_MAX_TEXTURES; i++ ) {
		slot = ( vk_nextSlot + i ) % VK_MAX_TEXTURES;
		if ( !vk_slots[slot].handle ) {
			// handles of a slot differ by its generation, so a handle is
			// only reused after 2^20 images went through the same slot
			vk_slots[slot].handle = vk_slotGeneration[slot] * VK_MAX_TEXTURES + slot + 1;
			vk_nextSlot = slot + 1;
			return slot;
		}
	}

	ri.Error( ERR_DROP, "Vulkan: more than %d textures", VK_MAX_TEXTURES );
	return -1;
}

static void VK_FreeSlot( int slot ) {
	vk_slots[slot].handle = 0;
	vk_slots[slot].texture = NULL;
	vk_slotGeneration[slot] = ( vk_slotGeneration[slot] + 1 ) % ( 0xFFFFFFFFu / VK_MAX_TEXTURES );
}

vkTexture_t *VK_GetTexture( unsigned int handle ) {
	int slot;

	slot = VK_SlotForHandle( handle );
	if ( slot < 0 ) {
		return NULL;
	}
	return vk_slots[slot].texture;
}

vkTexture_t *VK_DefaultTexture( void ) {
	return vk_defaultTexture;
}

/*
====================================================================

INTERFACE

====================================================================
*/

void vk_create_texture( image_t *image, int width, int height, int mipLevels, texFormat_t format ) {
	// r_smp: the front end's, with the render thread idle (the thread's own,
	// the ghost textures, go straight through)
	R_FrontEndSync();
	int slot;

	if ( !vk.device ) {
		return;
	}

	slot = VK_SlotForHandle( image->texnum );
	if ( slot < 0 ) {
		slot = VK_AllocSlot();
		image->texnum = vk_slots[slot].handle;
	} else if ( vk_slots[slot].texture ) {
		VK_DestroyTextureStorage( vk_slots[slot].texture );
	}

	if ( ( format >= TEXFMT_BC1_RGB ) && !vk.enabledFeatures.textureCompressionBC ) {
		// the renderer only loads DDS files when compression is reported
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: vk_create_texture: %s is compressed but the device has no BC formats\n", image->imgName );
		format = TEXFMT_RGBA8;
	}

	vk_slots[slot].texture = VK_CreateTextureStorage( width, height, mipLevels, format, image->wrapClampModeX, image->wrapClampModeY );
}

void vk_upload_texture( image_t *image, int mipLevel, int x, int y, int width, int height, const byte *rgba ) {
	R_FrontEndSync();
	vkTexture_t *texture;
	VkBuffer buffer;
	VkDeviceSize offset, size;
	int mipWidth, mipHeight;
	void *data;

	if ( !vk.device ) {
		return;
	}

	texture = VK_GetTexture( image->texnum );
	if ( !texture ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: vk_upload_texture: %s has no storage\n", image->imgName );
		return;
	}
	if ( texture->compressed ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: vk_upload_texture: %s is compressed\n", image->imgName );
		return;
	}
	if ( mipLevel < 0 || mipLevel >= texture->mipLevels ) {
		return;
	}

	mipWidth = Q_max( 1, texture->width >> mipLevel );
	mipHeight = Q_max( 1, texture->height >> mipLevel );
	if ( x < 0 || y < 0 || width <= 0 || height <= 0 || x + width > mipWidth || y + height > mipHeight ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: vk_upload_texture: %s: region %d,%d %dx%d outside of level %d (%dx%d)\n",
			image->imgName, x, y, width, height, mipLevel, mipWidth, mipHeight );
		return;
	}

	size = (VkDeviceSize)width * height * 4;
	data = VK_StagingAlloc( size, 16, &buffer, &offset );
	Com_Memcpy( data, rgba, size );

	VK_RecordUpload( texture, mipLevel, x, y, width, height, buffer, offset );
}

void vk_upload_compressed_texture( image_t *image, int mipLevel, int width, int height, const byte *data, int size ) {
	R_FrontEndSync();
	vkTexture_t *texture;
	VkBuffer buffer;
	VkDeviceSize offset, expected;
	int mipWidth, mipHeight;
	void *dst;

	if ( !vk.device ) {
		return;
	}

	texture = VK_GetTexture( image->texnum );
	if ( !texture ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: vk_upload_compressed_texture: %s has no storage\n", image->imgName );
		return;
	}
	if ( !texture->compressed ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: vk_upload_compressed_texture: %s is not compressed\n", image->imgName );
		return;
	}
	if ( mipLevel < 0 || mipLevel >= texture->mipLevels ) {
		return;
	}

	// always a whole level
	mipWidth = Q_max( 1, texture->width >> mipLevel );
	mipHeight = Q_max( 1, texture->height >> mipLevel );
	expected = (VkDeviceSize)VK_BlockSize( texture->format ) * ( ( mipWidth + 3 ) / 4 ) * ( ( mipHeight + 3 ) / 4 );
	if ( size < (int)expected ) {
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: vk_upload_compressed_texture: %s: level %d has %d bytes, %d expected\n",
			image->imgName, mipLevel, size, (int)expected );
		return;
	}

	dst = VK_StagingAlloc( expected, 16, &buffer, &offset );
	Com_Memcpy( dst, data, expected );

	VK_RecordUpload( texture, mipLevel, 0, 0, mipWidth, mipHeight, buffer, offset );
}

void vk_destroy_texture( image_t *image ) {
	R_FrontEndSync();
	int slot;

	slot = VK_SlotForHandle( image->texnum );
	image->texnum = 0;

	if ( slot < 0 || !vk.device ) {
		return;
	}

	if ( vk_slots[slot].texture ) {
		VK_DestroyTextureStorage( vk_slots[slot].texture );
	}
	VK_FreeSlot( slot );
}

void vk_set_texture_filter( texFilter_t filter ) {
	R_FrontEndSync();
	int i;

	if ( filter < TEXFILTER_NEAREST || filter > TEXFILTER_LINEAR_MIPMAP_LINEAR || filter == vk_textureFilter ) {
		return;
	}
	vk_textureFilter = filter;

	if ( !vk.device ) {
		return;
	}

	// the sampler is part of the descriptor set; sets in use by recorded
	// commands can't be updated, so mipmapped textures get new ones
	for ( i = 0; i < VK_MAX_TEXTURES; i++ ) {
		vkTexture_t *texture = vk_slots[i].texture;

		if ( !texture || texture->mipLevels <= 1 ) {
			continue;
		}
		VK_DeferDestroy( VK_DESTROY_DESCRIPTOR_SET, (uint64_t)texture->descriptorSet, (uint64_t)texture->descriptorPool );
		VK_AllocateDescriptorSet( texture );
	}
}

/*
====================================================================

INIT / SHUTDOWN

====================================================================
*/

void VK_InitTextures( void ) {
	static const byte black[4] = { 0, 0, 0, 255 };
	VkBuffer buffer;
	VkDeviceSize offset;
	void *data;

	// what a unit samples when its image has no storage, like an
	// incomplete GL texture
	vk_defaultTexture = VK_CreateTextureStorage( 1, 1, 1, TEXFMT_RGBA8, WRAP_REPEAT, WRAP_REPEAT );
	data = VK_StagingAlloc( 4, 16, &buffer, &offset );
	Com_Memcpy( data, black, 4 );
	VK_RecordUpload( vk_defaultTexture, 0, 0, 0, 1, 1, buffer, offset );
}

/*
===============
VK_ShutdownTextures

Called once the device is idle; the storages go to the deferred
destruction lists, which the caller processes before destroying the pools
===============
*/
void VK_ShutdownTextures( void ) {
	int i;

	// nothing will be submitted anymore: no layout transition to record
	for ( i = 0; i < vk_pendingUpload.num; i++ ) {
		vk_pendingUpload.textures[i]->uploadPending = qfalse;
	}
	for ( i = 0; i < vk_pendingMain.num; i++ ) {
		vk_pendingMain.textures[i]->mainPending = qfalse;
	}
	vk_pendingUpload.num = vk_pendingMain.num = 0;

	for ( i = 0; i < VK_MAX_TEXTURES; i++ ) {
		if ( vk_slots[i].texture ) {
			VK_DestroyTextureStorage( vk_slots[i].texture );
		}
	}
	Com_Memset( vk_slots, 0, sizeof( vk_slots ) );
	Com_Memset( vk_slotGeneration, 0, sizeof( vk_slotGeneration ) );
	vk_nextSlot = 0;

	if ( vk_defaultTexture ) {
		VK_DestroyTextureStorage( vk_defaultTexture );
		vk_defaultTexture = NULL;
	}

	VK_ListFree( &vk_pendingUpload );
	VK_ListFree( &vk_pendingMain );
}

void VK_ShutdownTextureObjects( void ) {
	int i, j, k;

	VK_ShutdownMemory();

	for ( i = 0; i < vk_numDescriptorPools; i++ ) {
		qvkDestroyDescriptorPool( vk.device, vk_descriptorPools[i], NULL );
	}
	vk_numDescriptorPools = 0;

	for ( i = 0; i <= VK_SAMPLER_NOMIP; i++ ) {
		for ( j = 0; j < 2; j++ ) {
			for ( k = 0; k < 2; k++ ) {
				if ( vk_samplers[i][j][k] ) {
					qvkDestroySampler( vk.device, vk_samplers[i][j][k], NULL );
					vk_samplers[i][j][k] = VK_NULL_HANDLE;
				}
			}
		}
	}

	vk_textureFilter = TEXFILTER_LINEAR_MIPMAP_NEAREST;
}
