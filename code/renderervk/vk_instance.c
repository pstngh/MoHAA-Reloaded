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

// vk_instance.c -- Vulkan loader, instance, device and swapchain

#include "vk_local.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

extern SDL_Window *SDL_window;

#define VK_DEFINE_FUNCTION( name ) PFN_##name q##name;
VK_GLOBAL_FUNCTIONS( VK_DEFINE_FUNCTION )
VK_INSTANCE_FUNCTIONS( VK_DEFINE_FUNCTION )
VK_INSTANCE_FUNCTIONS_OPTIONAL( VK_DEFINE_FUNCTION )
VK_DEVICE_FUNCTIONS( VK_DEFINE_FUNCTION )
VK_DEVICE_FUNCTIONS_OPTIONAL( VK_DEFINE_FUNCTION )
#undef VK_DEFINE_FUNCTION

static PFN_vkGetInstanceProcAddr qvkGetInstanceProcAddr;

// instance extensions that VK_KHR/EXT_swapchain_maintenance1 need, enabled
// when present
static qboolean vk_khrSurfaceMaintenance1, vk_extSurfaceMaintenance1;

#define VK_MAX_EXTENSIONS 24

/*
====================================================================

ERRORS

====================================================================
*/

const char *VK_ResultString( VkResult result ) {
	switch ( result ) {
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_NOT_READY: return "VK_NOT_READY";
	case VK_TIMEOUT: return "VK_TIMEOUT";
	case VK_INCOMPLETE: return "VK_INCOMPLETE";
	case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
	case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
	case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
	case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
	default: return va( "VkResult %d", (int)result );
	}
}

void VK_Check( VkResult result, const char *what ) {
	if ( result < 0 ) {
		ri.Error( ERR_FATAL, "Vulkan: %s failed: %s", what, VK_ResultString( result ) );
	}
}

// a monotonic clock for the vkinfo counters
uint64_t VK_Microseconds( void ) {
	static uint64_t frequency;

	if ( !frequency ) {
		frequency = SDL_GetPerformanceFrequency();
	}
	return SDL_GetPerformanceCounter() * 1000000.0 / frequency;
}

/*
====================================================================

VALIDATION

====================================================================
*/

static VKAPI_ATTR VkBool32 VKAPI_CALL VK_DebugCallback( VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT *data, void *userData )
{
	const char *level;

	if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ) {
		level = S_COLOR_RED "ERROR";
	} else if ( severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ) {
		level = S_COLOR_YELLOW "WARNING";
	} else {
		level = "info";
	}

	// raised on the submit thread: printed by the main thread (vk_submit.c),
	// formatted without va(), whose buffers aren't the thread's
	{
		char text[4096];

		SDL_snprintf( text, sizeof( text ), "Vulkan validation %s" S_COLOR_WHITE ": %s", level, data->pMessage );
		if ( VK_DeferThreadMessage( text ) ) {
			return VK_FALSE;
		}
	}

	// PRINT_WARNING only shows with developer set
	ri.Printf( PRINT_ALL, "Vulkan validation %s" S_COLOR_WHITE ": %s\n", level, data->pMessage );

	return VK_FALSE;
}

static void VK_FillDebugMessengerInfo( VkDebugUtilsMessengerCreateInfoEXT *info ) {
	Com_Memset( info, 0, sizeof( *info ) );
	info->sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	info->messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	info->messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
		| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	info->pfnUserCallback = VK_DebugCallback;
}

/*
====================================================================

INSTANCE

====================================================================
*/

static qboolean VK_HasExtension( const VkExtensionProperties *list, uint32_t count, const char *name ) {
	uint32_t i;

	for ( i = 0; i < count; i++ ) {
		if ( !strcmp( list[i].extensionName, name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

static void VK_AddExtension( const char **list, uint32_t *count, const char *name ) {
	uint32_t i;

	for ( i = 0; i < *count; i++ ) {
		if ( !strcmp( list[i], name ) ) {
			return;
		}
	}
	if ( *count == VK_MAX_EXTENSIONS ) {
		ri.Error( ERR_FATAL, "Vulkan: too many extensions" );
	}
	list[( *count )++] = name;
}

static void VK_CreateInstance( void ) {
	PFN_vkEnumerateInstanceVersion qvkEnumerateInstanceVersion;
	VkApplicationInfo appInfo;
	VkInstanceCreateInfo createInfo;
	VkDebugUtilsMessengerCreateInfoEXT debugInfo;
	VkValidationFeaturesEXT validationFeatures;
	VkValidationFeatureEnableEXT enabledValidation[1];
	VkExtensionProperties *available, *layerExtensions;
	uint32_t numAvailable, numLayerExtensions;
	const char *extensions[VK_MAX_EXTENSIONS];
	const char * const *sdlExtensions;
	uint32_t numExtensions;
	Uint32 numSdlExtensions;
	const char *layers[1];
	uint32_t numLayers;
	uint32_t apiVersion;
	qboolean debugUtils;
	unsigned int i;
	VkResult result;

	qvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
	if ( !qvkGetInstanceProcAddr ) {
		ri.Error( ERR_FATAL, "Vulkan: no Vulkan loader (SDL_Vulkan_GetVkGetInstanceProcAddr: %s)" VK_DRIVER_HINT, SDL_GetError() );
	}

#define VK_LOAD_GLOBAL( name ) \
	q##name = (PFN_##name)qvkGetInstanceProcAddr( VK_NULL_HANDLE, #name ); \
	if ( !q##name ) ri.Error( ERR_FATAL, "Vulkan: can't load %s", #name );
	VK_GLOBAL_FUNCTIONS( VK_LOAD_GLOBAL )
#undef VK_LOAD_GLOBAL

	apiVersion = VK_API_VERSION_1_0;
	qvkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)qvkGetInstanceProcAddr( VK_NULL_HANDLE, "vkEnumerateInstanceVersion" );
	if ( qvkEnumerateInstanceVersion ) {
		qvkEnumerateInstanceVersion( &apiVersion );
	}
	if ( apiVersion < VK_API_VERSION_1_1 ) {
		ri.Error( ERR_FATAL, "Vulkan: the Vulkan loader only supports version %d.%d, 1.1 is required",
			VK_API_VERSION_MAJOR( apiVersion ), VK_API_VERSION_MINOR( apiVersion ) );
	}

	// extensions SDL needs for the window surface (SDL's array)
	sdlExtensions = SDL_Vulkan_GetInstanceExtensions( &numSdlExtensions );
	if ( !sdlExtensions ) {
		ri.Error( ERR_FATAL, "Vulkan: SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError() );
	}

	numExtensions = 0;
	for ( i = 0; i < numSdlExtensions; i++ ) {
		VK_AddExtension( extensions, &numExtensions, sdlExtensions[i] );
	}

	VK_CHECK( qvkEnumerateInstanceExtensionProperties( NULL, &numAvailable, NULL ) );
	available = ri.Hunk_AllocateTempMemory( sizeof( *available ) * ( numAvailable + 1 ) );
	VK_CHECK( qvkEnumerateInstanceExtensionProperties( NULL, &numAvailable, available ) );

	createInfo.flags = 0;
	if ( VK_HasExtension( available, numAvailable, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME ) ) {
		// MoltenVK only shows up with this
		VK_AddExtension( extensions, &numExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME );
		createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}

	// present fences (VK_KHR/EXT_swapchain_maintenance1, see
	// VK_CreateLogicalDevice): their surface extensions
	vk_khrSurfaceMaintenance1 = vk_extSurfaceMaintenance1 = qfalse;
	if ( !( vk.fallbacks & VK_FALLBACK_PRESENT_FENCES )
		&& VK_HasExtension( available, numAvailable, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME ) ) {
		if ( VK_HasExtension( available, numAvailable, "VK_KHR_surface_maintenance1" ) ) {
			vk_khrSurfaceMaintenance1 = qtrue;
		}
		if ( VK_HasExtension( available, numAvailable, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME ) ) {
			vk_extSurfaceMaintenance1 = qtrue;
		}
		if ( vk_khrSurfaceMaintenance1 || vk_extSurfaceMaintenance1 ) {
			VK_AddExtension( extensions, &numExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME );
		}
		if ( vk_khrSurfaceMaintenance1 ) {
			VK_AddExtension( extensions, &numExtensions, "VK_KHR_surface_maintenance1" );
		}
		if ( vk_extSurfaceMaintenance1 ) {
			VK_AddExtension( extensions, &numExtensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME );
		}
	}

	// HDR output (r_hdrOutput, vk_output.c): the HDR10 and scRGB color spaces
	// of the surface formats
	vko.enabled = qfalse;
	vko.sdrPass = VK_OutputSdrPassWanted();
	if ( VK_OutputWanted() ) {
		if ( VK_HasExtension( available, numAvailable, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME ) ) {
			VK_AddExtension( extensions, &numExtensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME );
			vko.enabled = qtrue;
		} else {
			ri.Printf( PRINT_ALL, S_COLOR_YELLOW "HDR output: off, the Vulkan loader has no %s\n", VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME );
		}
	}

	// validation layer
	numLayers = 0;
	debugUtils = qfalse;
	layerExtensions = NULL;
	numLayerExtensions = 0;
	if ( r_vkValidation->integer ) {
		VkLayerProperties *layerList;
		uint32_t numLayerList;

		VK_CHECK( qvkEnumerateInstanceLayerProperties( &numLayerList, NULL ) );
		layerList = ri.Hunk_AllocateTempMemory( sizeof( *layerList ) * ( numLayerList + 1 ) );
		VK_CHECK( qvkEnumerateInstanceLayerProperties( &numLayerList, layerList ) );
		for ( i = 0; i < numLayerList; i++ ) {
			if ( !strcmp( layerList[i].layerName, "VK_LAYER_KHRONOS_validation" ) ) {
				break;
			}
		}
		if ( i < numLayerList ) {
			layers[numLayers++] = "VK_LAYER_KHRONOS_validation";

			VK_CHECK( qvkEnumerateInstanceExtensionProperties( "VK_LAYER_KHRONOS_validation", &numLayerExtensions, NULL ) );
			layerExtensions = ri.Hunk_AllocateTempMemory( sizeof( *layerExtensions ) * ( numLayerExtensions + 1 ) );
			VK_CHECK( qvkEnumerateInstanceExtensionProperties( "VK_LAYER_KHRONOS_validation", &numLayerExtensions, layerExtensions ) );
		} else {
			ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: r_vkValidation: VK_LAYER_KHRONOS_validation is not installed\n" );
		}
		ri.Hunk_FreeTempMemory( layerList );

		if ( VK_HasExtension( available, numAvailable, VK_EXT_DEBUG_UTILS_EXTENSION_NAME )
			|| VK_HasExtension( layerExtensions, numLayerExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME ) ) {
			VK_AddExtension( extensions, &numExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
			debugUtils = qtrue;
		} else {
			ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: r_vkValidation: %s is not available, validation messages can't be printed\n", VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
		}
	}

	Com_Memset( &appInfo, 0, sizeof( appInfo ) );
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = PRODUCT_NAME;
	appInfo.applicationVersion = 1;
	appInfo.pEngineName = PRODUCT_NAME;
	appInfo.engineVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_1;

	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pNext = NULL;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledLayerCount = numLayers;
	createInfo.ppEnabledLayerNames = layers;
	createInfo.enabledExtensionCount = numExtensions;
	createInfo.ppEnabledExtensionNames = extensions;

	if ( debugUtils ) {
		// also reports problems of vkCreateInstance and vkDestroyInstance
		VK_FillDebugMessengerInfo( &debugInfo );
		createInfo.pNext = &debugInfo;

		// r_vkValidation 2 also checks synchronization
		if ( r_vkValidation->integer >= 2 ) {
			if ( VK_HasExtension( layerExtensions, numLayerExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME ) ) {
				VK_AddExtension( extensions, &numExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME );
				createInfo.enabledExtensionCount = numExtensions;

				enabledValidation[0] = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
				Com_Memset( &validationFeatures, 0, sizeof( validationFeatures ) );
				validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
				validationFeatures.enabledValidationFeatureCount = ARRAY_LEN( enabledValidation );
				validationFeatures.pEnabledValidationFeatures = enabledValidation;
				debugInfo.pNext = &validationFeatures;
			} else {
				ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: r_vkValidation 2: %s is not available\n", VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME );
			}
		}
	}

	result = qvkCreateInstance( &createInfo, NULL, &vk.instance );
	if ( result == VK_ERROR_INCOMPATIBLE_DRIVER ) {
		ri.Error( ERR_FATAL, "Vulkan: no compatible Vulkan driver found" VK_DRIVER_HINT );
	}
	VK_CHECK( result );

	if ( layerExtensions ) {
		ri.Hunk_FreeTempMemory( layerExtensions );
	}
	ri.Hunk_FreeTempMemory( available );

#define VK_LOAD_INSTANCE( name ) \
	q##name = (PFN_##name)qvkGetInstanceProcAddr( vk.instance, #name ); \
	if ( !q##name ) ri.Error( ERR_FATAL, "Vulkan: can't load %s", #name );
	VK_INSTANCE_FUNCTIONS( VK_LOAD_INSTANCE )
#undef VK_LOAD_INSTANCE

#define VK_LOAD_INSTANCE_OPTIONAL( name ) \
	q##name = (PFN_##name)qvkGetInstanceProcAddr( vk.instance, #name );
	VK_INSTANCE_FUNCTIONS_OPTIONAL( VK_LOAD_INSTANCE_OPTIONAL )
#undef VK_LOAD_INSTANCE_OPTIONAL

	if ( debugUtils && qvkCreateDebugUtilsMessengerEXT ) {
		VK_FillDebugMessengerInfo( &debugInfo );
		VK_CHECK( qvkCreateDebugUtilsMessengerEXT( vk.instance, &debugInfo, NULL, &vk.debugMessenger ) );
		ri.Printf( PRINT_ALL, "Vulkan validation enabled%s\n", r_vkValidation->integer >= 2 ? " (with synchronization validation)" : "" );
	}
}

/*
====================================================================

DEVICE

====================================================================
*/

// returns a score for the device, 0 if it can't be used
static int VK_RateDevice( VkPhysicalDevice device, uint32_t *queueFamily ) {
	VkPhysicalDeviceProperties props;
	VkQueueFamilyProperties *families;
	VkExtensionProperties *extensions;
	uint32_t numFamilies, numExtensions, i;
	qboolean hasSwapchain;
	int score;

	qvkGetPhysicalDeviceProperties( device, &props );
	if ( props.apiVersion < VK_API_VERSION_1_1 ) {
		ri.Printf( PRINT_ALL, "...%s: Vulkan %d.%d, 1.1 is required\n", props.deviceName,
			VK_API_VERSION_MAJOR( props.apiVersion ), VK_API_VERSION_MINOR( props.apiVersion ) );
		return 0;
	}

	VK_CHECK( qvkEnumerateDeviceExtensionProperties( device, NULL, &numExtensions, NULL ) );
	extensions = ri.Hunk_AllocateTempMemory( sizeof( *extensions ) * ( numExtensions + 1 ) );
	VK_CHECK( qvkEnumerateDeviceExtensionProperties( device, NULL, &numExtensions, extensions ) );
	hasSwapchain = VK_HasExtension( extensions, numExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME );
	ri.Hunk_FreeTempMemory( extensions );
	if ( !hasSwapchain ) {
		ri.Printf( PRINT_ALL, "...%s: no %s\n", props.deviceName, VK_KHR_SWAPCHAIN_EXTENSION_NAME );
		return 0;
	}

	// one queue for graphics, transfers and presentation
	qvkGetPhysicalDeviceQueueFamilyProperties( device, &numFamilies, NULL );
	families = ri.Hunk_AllocateTempMemory( sizeof( *families ) * ( numFamilies + 1 ) );
	qvkGetPhysicalDeviceQueueFamilyProperties( device, &numFamilies, families );
	for ( i = 0; i < numFamilies; i++ ) {
		VkBool32 present = VK_FALSE;

		if ( !( families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ) || !families[i].queueCount ) {
			continue;
		}
		VK_CHECK( qvkGetPhysicalDeviceSurfaceSupportKHR( device, i, vk.surface, &present ) );
		if ( present ) {
			break;
		}
	}
	ri.Hunk_FreeTempMemory( families );
	if ( i == numFamilies ) {
		ri.Printf( PRINT_ALL, "...%s: can't present to the window\n", props.deviceName );
		return 0;
	}
	*queueFamily = i;

	switch ( props.deviceType ) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
		score = 5;
		break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
		score = 4;
		break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
		score = 3;
		break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:
		score = 1;
		break;
	default:
		score = 2;
		break;
	}
	return score;
}

static void VK_SelectPhysicalDevice( void ) {
	VkPhysicalDevice *devices;
	uint32_t numDevices, i, queueFamily;
	int score, bestScore;

	VK_CHECK( qvkEnumeratePhysicalDevices( vk.instance, &numDevices, NULL ) );
	if ( !numDevices ) {
		ri.Error( ERR_FATAL, "Vulkan: no Vulkan device found" VK_DRIVER_HINT );
	}
	devices = ri.Hunk_AllocateTempMemory( sizeof( *devices ) * numDevices );
	VK_CHECK( qvkEnumeratePhysicalDevices( vk.instance, &numDevices, devices ) );

	bestScore = 0;
	for ( i = 0; i < numDevices; i++ ) {
		score = VK_RateDevice( devices[i], &queueFamily );
		if ( score > bestScore ) {
			bestScore = score;
			vk.physicalDevice = devices[i];
			vk.queueFamily = queueFamily;
		}
	}
	ri.Hunk_FreeTempMemory( devices );

	if ( !bestScore ) {
		ri.Error( ERR_FATAL, "Vulkan: no usable Vulkan device found" VK_DRIVER_HINT );
	}

	qvkGetPhysicalDeviceProperties( vk.physicalDevice, &vk.deviceProperties );
	qvkGetPhysicalDeviceMemoryProperties( vk.physicalDevice, &vk.memoryProperties );

	// GPU timestamps (vkinfo), and a compute-only family for the ambient
	// occlusion's passes (vk_temporal.c): one without graphics, so its queue
	// runs beside the graphics one
	{
		VkQueueFamilyProperties *families;
		uint32_t numFamilies, i;

		qvkGetPhysicalDeviceQueueFamilyProperties( vk.physicalDevice, &numFamilies, NULL );
		families = ri.Hunk_AllocateTempMemory( sizeof( *families ) * ( numFamilies + 1 ) );
		qvkGetPhysicalDeviceQueueFamilyProperties( vk.physicalDevice, &numFamilies, families );
		vk.timestampValidBits = vk.deviceProperties.limits.timestampPeriod > 0 ? families[vk.queueFamily].timestampValidBits : 0;
		vk.computeFamily = VK_QUEUE_FAMILY_IGNORED;
		for ( i = 0; i < numFamilies; i++ ) {
			if ( ( families[i].queueFlags & VK_QUEUE_COMPUTE_BIT ) && !( families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT )
				&& families[i].queueCount && i != vk.queueFamily ) {
				vk.computeFamily = i;
				break;
			}
		}
		ri.Hunk_FreeTempMemory( families );
	}
}

// the features of an extension's feature structure (pNext NULL) that the
// device supports
static void VK_GetExtensionFeatures( void *features ) {
	VkPhysicalDeviceFeatures2 features2;

	Com_Memset( &features2, 0, sizeof( features2 ) );
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = features;
	qvkGetPhysicalDeviceFeatures2( vk.physicalDevice, &features2 );
}

static void VK_CreateLogicalDevice( void ) {
	VkPhysicalDeviceFeatures supported;
	VkPhysicalDeviceDepthClipControlFeaturesEXT clipControl;
	VkPhysicalDeviceLineRasterizationFeaturesEXT lineFeatures;
	VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchainFeatures;
	VkPhysicalDevicePortabilitySubsetFeaturesKHR portabilityFeatures;
	VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineFeatures;
	VkPhysicalDevicePresentIdFeaturesKHR presentIdFeatures;
	VkPhysicalDevicePresentWaitFeaturesKHR presentWaitFeatures;
	qboolean portability;
	const char *lineExtension, *swapchainExtension;
	VkDeviceQueueCreateInfo queueInfos[2];
	VkDeviceCreateInfo createInfo;
	VkExtensionProperties *available;
	const char *extensions[VK_MAX_EXTENSIONS];
	uint32_t numAvailable, numExtensions;
	const float priority = 1.0f;
	char *s;
	uint32_t i;

	qvkGetPhysicalDeviceFeatures( vk.physicalDevice, &supported );

	// only what the renderer uses; all optional
	Com_Memset( &vk.enabledFeatures, 0, sizeof( vk.enabledFeatures ) );
	vk.enabledFeatures.fillModeNonSolid = supported.fillModeNonSolid;		// GLS_POLYMODE_LINE
	vk.enabledFeatures.wideLines = supported.wideLines;					// debug lines
	vk.enabledFeatures.samplerAnisotropy = supported.samplerAnisotropy;
	vk.enabledFeatures.textureCompressionBC = supported.textureCompressionBC;	// DDS textures
	vk.enabledFeatures.shaderClipDistance = supported.shaderClipDistance;	// portal clip plane
	vk.enabledFeatures.depthClamp = supported.depthClamp;					// solid surfaces nearer than r_znear (VK_PIPE_DEPTHCLAMP)
	// bindless textures index the array of every texture slot by a value
	// uniform within the draw (vk.bindless)
	vk.enabledFeatures.shaderSampledImageArrayDynamicIndexing = supported.shaderSampledImageArrayDynamicIndexing;
	// the temporal path's storage images (RG32F, RG16F, A2B10G10R10) and the
	// blend states of its three color attachments (vk_temporal.c)
	vk.enabledFeatures.shaderStorageImageExtendedFormats = supported.shaderStorageImageExtendedFormats;
	vk.enabledFeatures.independentBlend = supported.independentBlend;

	VK_CHECK( qvkEnumerateDeviceExtensionProperties( vk.physicalDevice, NULL, &numAvailable, NULL ) );
	available = ri.Hunk_AllocateTempMemory( sizeof( *available ) * ( numAvailable + 1 ) );
	VK_CHECK( qvkEnumerateDeviceExtensionProperties( vk.physicalDevice, NULL, &numAvailable, available ) );

	numExtensions = 0;
	VK_AddExtension( extensions, &numExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME );

	// must be enabled when the device has it (MoltenVK). Of what it can
	// leave out, the renderer uses the A = ONE view swizzle of RGB textures
	// (imageViewFormatSwizzle; without it the fragment shader sets the
	// alpha, see vk.viewSwizzle) and vertex bindings with a stride of 0
	// (constant colors, unused texture coordinates), which it never allows:
	// a stride must be at least minVertexInputBindingStrideAlignment
	// (VUID-VkVertexInputBindingDescription-stride-04456), whatever
	// vertexAttributeAccessBeyondStride says, so those bindings are read per
	// instance with a stride of their attribute's size (see vk.strideZero),
	// which needs neither
	Com_Memset( &portabilityFeatures, 0, sizeof( portabilityFeatures ) );
	portabilityFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
	vk.viewSwizzle = qtrue;
	vk.strideZero = qtrue;
	portability = VK_HasExtension( available, numAvailable, "VK_KHR_portability_subset" );
	if ( portability ) {
		VK_AddExtension( extensions, &numExtensions, "VK_KHR_portability_subset" );
		if ( qvkGetPhysicalDeviceFeatures2 ) {
			VkPhysicalDevicePortabilitySubsetFeaturesKHR supportedPortability;

			Com_Memset( &supportedPortability, 0, sizeof( supportedPortability ) );
			supportedPortability.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
			VK_GetExtensionFeatures( &supportedPortability );
			portabilityFeatures.imageViewFormatSwizzle = supportedPortability.imageViewFormatSwizzle;
		}
		if ( qvkGetPhysicalDeviceProperties2 ) {
			VkPhysicalDevicePortabilitySubsetPropertiesKHR portabilityProps;
			VkPhysicalDeviceProperties2 props2;

			Com_Memset( &portabilityProps, 0, sizeof( portabilityProps ) );
			portabilityProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_PROPERTIES_KHR;
			Com_Memset( &props2, 0, sizeof( props2 ) );
			props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			props2.pNext = &portabilityProps;
			qvkGetPhysicalDeviceProperties2( vk.physicalDevice, &props2 );
			// the strides are 4 (colors), 8, 12 (positions) and 16 bytes
			if ( portabilityProps.minVertexInputBindingStrideAlignment > 4 ) {
				ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: Vulkan: minVertexInputBindingStrideAlignment is %u, vertex strides of 4 and 12 bytes aren't allowed\n",
					portabilityProps.minVertexInputBindingStrideAlignment );
			}
		}
		vk.viewSwizzle = portabilityFeatures.imageViewFormatSwizzle ? qtrue : qfalse;
		vk.strideZero = qfalse;
	}
	if ( vk.fallbacks & VK_FALLBACK_VIEW_SWIZZLE ) {
		vk.viewSwizzle = qfalse;
	}
	if ( vk.fallbacks & VK_FALLBACK_STRIDE_ZERO ) {
		vk.strideZero = qfalse;
	}

	// present fences: a present signals a fence once its wait semaphore can
	// be reused or destroyed, and once those of every present to a swapchain
	// have signaled, the swapchain can be destroyed (see VK_DestroyRetiredSwapchains)
	Com_Memset( &swapchainFeatures, 0, sizeof( swapchainFeatures ) );
	swapchainFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
	vk.presentFences = qfalse;
	swapchainExtension = NULL;
	if ( vk_khrSurfaceMaintenance1 && VK_HasExtension( available, numAvailable, "VK_KHR_swapchain_maintenance1" ) ) {
		swapchainExtension = "VK_KHR_swapchain_maintenance1";
	} else if ( vk_extSurfaceMaintenance1 && VK_HasExtension( available, numAvailable, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME ) ) {
		swapchainExtension = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
	}
	if ( qvkGetPhysicalDeviceFeatures2 && swapchainExtension ) {
		VK_GetExtensionFeatures( &swapchainFeatures );
		if ( swapchainFeatures.swapchainMaintenance1 ) {
			VK_AddExtension( extensions, &numExtensions, swapchainExtension );
			vk.presentFences = qtrue;
		}
	}

	// GL clip space depth: the projection is used as is, so clip space z,
	// and with it near/far clipping and the depth values, are the same as GL's
	Com_Memset( &clipControl, 0, sizeof( clipControl ) );
	clipControl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT;
	vk.depthClipControl = qfalse;
	if ( qvkGetPhysicalDeviceFeatures2 && VK_HasExtension( available, numAvailable, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME ) ) {
		VK_GetExtensionFeatures( &clipControl );
		if ( clipControl.depthClipControl ) {
			VK_AddExtension( extensions, &numExtensions, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME );
			vk.depthClipControl = qtrue;
		}
	}

	// GL rasterizes aliased lines with the diamond-exit rule, which is
	// Vulkan's Bresenham mode; it also provides line stipple (debug lines).
	// Without it lines have the device's default rule and no stipple
	Com_Memset( &lineFeatures, 0, sizeof( lineFeatures ) );
	lineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
	vk.bresenhamLines = qfalse;
	vk.lineStipple = qfalse;
	lineExtension = NULL;
	if ( vk.fallbacks & VK_FALLBACK_LINES ) {
		// none
	} else if ( VK_HasExtension( available, numAvailable, "VK_KHR_line_rasterization" ) ) {
		lineExtension = "VK_KHR_line_rasterization";
	} else if ( VK_HasExtension( available, numAvailable, VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME ) ) {
		lineExtension = VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME;
	}
	if ( qvkGetPhysicalDeviceFeatures2 && lineExtension ) {
		VkPhysicalDeviceLineRasterizationFeaturesEXT supportedLines;

		Com_Memset( &supportedLines, 0, sizeof( supportedLines ) );
		supportedLines.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT;
		VK_GetExtensionFeatures( &supportedLines );
		if ( supportedLines.bresenhamLines ) {
			VK_AddExtension( extensions, &numExtensions, lineExtension );
			lineFeatures.bresenhamLines = VK_TRUE;
			lineFeatures.stippledBresenhamLines = supportedLines.stippledBresenhamLines;
			vk.bresenhamLines = qtrue;
			vk.lineStipple = supportedLines.stippledBresenhamLines ? qtrue : qfalse;
		}
	}
	// multisampling resolves the depth/stencil buffer too, so readbacks
	// (screenshots of stencil, flares) keep working
	vk.depthStencilResolve = qfalse;
	if ( VK_HasExtension( available, numAvailable, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME )
		&& VK_HasExtension( available, numAvailable, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME ) ) {
		VK_AddExtension( extensions, &numExtensions, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME );
		VK_AddExtension( extensions, &numExtensions, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME );
		vk.depthStencilResolve = qtrue;
	}
	// HDR output: the display's luminance for the compositor
	vko.metadata = qfalse;
	if ( vko.enabled && VK_HasExtension( available, numAvailable, VK_EXT_HDR_METADATA_EXTENSION_NAME ) ) {
		VK_AddExtension( extensions, &numExtensions, VK_EXT_HDR_METADATA_EXTENSION_NAME );
		vko.metadata = qtrue;
	}

	// a compute queue and timeline semaphores for the ambient occlusion's
	// passes beside the next frame's rendering (r_aoAsync, vk_temporal.c)
	Com_Memset( &timelineFeatures, 0, sizeof( timelineFeatures ) );
	timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
	vk.asyncCompute = qfalse;
	if ( vk.computeFamily != VK_QUEUE_FAMILY_IGNORED && qvkGetPhysicalDeviceFeatures2
		&& VK_HasExtension( available, numAvailable, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME ) ) {
		VK_GetExtensionFeatures( &timelineFeatures );
		if ( timelineFeatures.timelineSemaphore ) {
			VK_AddExtension( extensions, &numExtensions, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME );
			vk.asyncCompute = qtrue;
		}
	}
	// present ids and waits: with FIFO a frame reads its input once the one
	// before is shown (vk.c, VK_WaitForPresent)
	Com_Memset( &presentIdFeatures, 0, sizeof( presentIdFeatures ) );
	presentIdFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
	Com_Memset( &presentWaitFeatures, 0, sizeof( presentWaitFeatures ) );
	presentWaitFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
	vk.presentWait = qfalse;
	if ( qvkGetPhysicalDeviceFeatures2 && VK_HasExtension( available, numAvailable, VK_KHR_PRESENT_ID_EXTENSION_NAME )
		&& VK_HasExtension( available, numAvailable, VK_KHR_PRESENT_WAIT_EXTENSION_NAME ) ) {
		VK_GetExtensionFeatures( &presentIdFeatures );
		VK_GetExtensionFeatures( &presentWaitFeatures );
		if ( presentIdFeatures.presentId && presentWaitFeatures.presentWait ) {
			VK_AddExtension( extensions, &numExtensions, VK_KHR_PRESENT_ID_EXTENSION_NAME );
			VK_AddExtension( extensions, &numExtensions, VK_KHR_PRESENT_WAIT_EXTENSION_NAME );
			vk.presentWait = qtrue;
		}
	}
	// the GPU's timestamps on the CPU's clock, for the latency probe
	// (r_vkLatency): the device's and the clock SDL's performance counter
	// reads (CLOCK_MONOTONIC_RAW on Linux, QueryPerformanceCounter on Windows)
	vk.calibrationDomain = VK_TIME_DOMAIN_MAX_ENUM_KHR;
	if ( qvkGetPhysicalDeviceCalibrateableTimeDomainsKHR && vk.timestampValidBits ) {
		const char *name = VK_HasExtension( available, numAvailable, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME ) ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
			: VK_HasExtension( available, numAvailable, VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME ) ? VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME : NULL;
		VkTimeDomainKHR domains[8];
		uint32_t numDomains = ARRAY_LEN( domains ), d;
		qboolean device = qfalse;
#ifdef _WIN32
		VkTimeDomainKHR wanted = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
#else
		VkTimeDomainKHR wanted = VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR;
#endif
		if ( name && qvkGetPhysicalDeviceCalibrateableTimeDomainsKHR( vk.physicalDevice, &numDomains, domains ) >= VK_SUCCESS ) {
			for ( d = 0; d < numDomains; d++ ) {
				if ( domains[d] == VK_TIME_DOMAIN_DEVICE_KHR ) {
					device = qtrue;
				}
			}
			for ( d = 0; d < numDomains && device; d++ ) {
				if ( domains[d] == wanted ) {
					VK_AddExtension( extensions, &numExtensions, name );
					vk.calibrationDomain = wanted;
				}
			}
		}
	}
	ri.Hunk_FreeTempMemory( available );

	Com_Memset( queueInfos, 0, sizeof( queueInfos ) );
	queueInfos[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfos[0].queueFamilyIndex = vk.queueFamily;
	queueInfos[0].queueCount = 1;
	queueInfos[0].pQueuePriorities = &priority;
	queueInfos[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfos[1].queueFamilyIndex = vk.computeFamily;
	queueInfos[1].queueCount = 1;
	queueInfos[1].pQueuePriorities = &priority;

	Com_Memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = vk.asyncCompute ? 2 : 1;
	createInfo.pQueueCreateInfos = queueInfos;
	createInfo.enabledExtensionCount = numExtensions;
	createInfo.ppEnabledExtensionNames = extensions;
	createInfo.pEnabledFeatures = &vk.enabledFeatures;
	if ( vk.bresenhamLines ) {
		lineFeatures.pNext = (void *)createInfo.pNext;
		createInfo.pNext = &lineFeatures;
	}
	if ( vk.depthClipControl ) {
		clipControl.pNext = (void *)createInfo.pNext;
		createInfo.pNext = &clipControl;
	}
	if ( vk.presentFences ) {
		swapchainFeatures.pNext = (void *)createInfo.pNext;
		createInfo.pNext = &swapchainFeatures;
	}
	if ( vk.asyncCompute ) {
		timelineFeatures.timelineSemaphore = VK_TRUE;
		timelineFeatures.pNext = (void *)createInfo.pNext;
		createInfo.pNext = &timelineFeatures;
	}
	if ( vk.presentWait ) {
		presentIdFeatures.presentId = VK_TRUE;
		presentIdFeatures.pNext = (void *)createInfo.pNext;
		createInfo.pNext = &presentIdFeatures;
		presentWaitFeatures.presentWait = VK_TRUE;
		presentWaitFeatures.pNext = (void *)createInfo.pNext;
		createInfo.pNext = &presentWaitFeatures;
	}
	if ( portability ) {
		portabilityFeatures.pNext = (void *)createInfo.pNext;
		createInfo.pNext = &portabilityFeatures;
	}

	VK_CHECK( qvkCreateDevice( vk.physicalDevice, &createInfo, NULL, &vk.device ) );

#define VK_LOAD_DEVICE( name ) \
	q##name = (PFN_##name)qvkGetDeviceProcAddr( vk.device, #name ); \
	if ( !q##name ) ri.Error( ERR_FATAL, "Vulkan: can't load %s", #name );
	VK_DEVICE_FUNCTIONS( VK_LOAD_DEVICE )
#undef VK_LOAD_DEVICE

	qvkCmdSetLineStippleEXT = NULL;
	if ( vk.lineStipple ) {
		qvkCmdSetLineStippleEXT = (PFN_vkCmdSetLineStippleEXT)qvkGetDeviceProcAddr( vk.device,
			!strcmp( lineExtension, "VK_KHR_line_rasterization" ) ? "vkCmdSetLineStippleKHR" : "vkCmdSetLineStippleEXT" );
		if ( !qvkCmdSetLineStippleEXT ) {
			vk.lineStipple = qfalse;
		}
	}

	qvkReleaseSwapchainImagesEXT = NULL;
	if ( vk.presentFences ) {
		qvkReleaseSwapchainImagesEXT = (PFN_vkReleaseSwapchainImagesEXT)qvkGetDeviceProcAddr( vk.device,
			!strcmp( swapchainExtension, "VK_KHR_swapchain_maintenance1" ) ? "vkReleaseSwapchainImagesKHR" : "vkReleaseSwapchainImagesEXT" );
	}

	qvkCreateRenderPass2KHR = NULL;
	if ( vk.depthStencilResolve ) {
		qvkCreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR)qvkGetDeviceProcAddr( vk.device, "vkCreateRenderPass2KHR" );
		if ( !qvkCreateRenderPass2KHR ) {
			vk.depthStencilResolve = qfalse;
		}
	}

	qvkGetDeviceQueue( vk.device, vk.queueFamily, 0, &vk.queue );

	qvkGetCalibratedTimestampsKHR = NULL;
	if ( vk.calibrationDomain != VK_TIME_DOMAIN_MAX_ENUM_KHR ) {
		qvkGetCalibratedTimestampsKHR = (PFN_vkGetCalibratedTimestampsKHR)qvkGetDeviceProcAddr( vk.device, "vkGetCalibratedTimestampsKHR" );
		if ( !qvkGetCalibratedTimestampsKHR ) {
			qvkGetCalibratedTimestampsKHR = (PFN_vkGetCalibratedTimestampsKHR)qvkGetDeviceProcAddr( vk.device, "vkGetCalibratedTimestampsEXT" );
		}
		if ( !qvkGetCalibratedTimestampsKHR ) {
			vk.calibrationDomain = VK_TIME_DOMAIN_MAX_ENUM_KHR;
		}
	}

	qvkWaitForPresentKHR = NULL;
	if ( vk.presentWait ) {
		qvkWaitForPresentKHR = (PFN_vkWaitForPresentKHR)qvkGetDeviceProcAddr( vk.device, "vkWaitForPresentKHR" );
		if ( !qvkWaitForPresentKHR ) {
			vk.presentWait = qfalse;
		}
	}

	qvkWaitSemaphoresKHR = NULL;
	qvkSignalSemaphoreKHR = NULL;
	qvkGetSemaphoreCounterValueKHR = NULL;
	vk.computeQueue = VK_NULL_HANDLE;
	if ( vk.asyncCompute ) {
		qvkWaitSemaphoresKHR = (PFN_vkWaitSemaphoresKHR)qvkGetDeviceProcAddr( vk.device, "vkWaitSemaphoresKHR" );
		qvkSignalSemaphoreKHR = (PFN_vkSignalSemaphoreKHR)qvkGetDeviceProcAddr( vk.device, "vkSignalSemaphoreKHR" );
		qvkGetSemaphoreCounterValueKHR = (PFN_vkGetSemaphoreCounterValueKHR)qvkGetDeviceProcAddr( vk.device, "vkGetSemaphoreCounterValueKHR" );
		if ( qvkWaitSemaphoresKHR && qvkSignalSemaphoreKHR && qvkGetSemaphoreCounterValueKHR ) {
			qvkGetDeviceQueue( vk.device, vk.computeFamily, 0, &vk.computeQueue );
		} else {
			vk.asyncCompute = qfalse;
		}
	}

	// reported by gfxinfo
	s = glConfig.extensions_string;
	s[0] = 0;
	for ( i = 0; i < numExtensions; i++ ) {
		Q_strcat( s, sizeof( glConfig.extensions_string ), extensions[i] );
		Q_strcat( s, sizeof( glConfig.extensions_string ), " " );
	}
}

void VK_CreateDevice( void ) {
	ri.Printf( PRINT_ALL, "Initializing Vulkan\n" );

	VK_CreateInstance();

	if ( !SDL_Vulkan_CreateSurface( SDL_window, vk.instance, NULL, &vk.surface ) ) {
		ri.Error( ERR_FATAL, "Vulkan: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError() );
	}

	VK_SelectPhysicalDevice();
	VK_CreateLogicalDevice();
}

void VK_DestroyDevice( void ) {
	if ( vk.device ) {
		qvkDestroyDevice( vk.device, NULL );
		vk.device = VK_NULL_HANDLE;
	}
	if ( vk.surface ) {
		qvkDestroySurfaceKHR( vk.instance, vk.surface, NULL );
		vk.surface = VK_NULL_HANDLE;
	}
	if ( vk.debugMessenger ) {
		qvkDestroyDebugUtilsMessengerEXT( vk.instance, vk.debugMessenger, NULL );
		vk.debugMessenger = VK_NULL_HANDLE;
	}
	if ( vk.instance ) {
		qvkDestroyInstance( vk.instance, NULL );
		vk.instance = VK_NULL_HANDLE;
	}
}

/*
====================================================================

SWAPCHAIN

====================================================================
*/

static qboolean VK_FormatSupportsBlitDst( VkFormat format ) {
	VkFormatProperties props;

	qvkGetPhysicalDeviceFormatProperties( vk.physicalDevice, format, &props );
	return ( props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT ) != 0;
}

static VkSurfaceFormatKHR VK_ChooseSurfaceFormat( void ) {
	static const VkFormat preferred[] = {
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_A8B8G8R8_UNORM_PACK32
	};
	VkSurfaceFormatKHR *formats, chosen;
	uint32_t numFormats, i, j;

	VK_CHECK( qvkGetPhysicalDeviceSurfaceFormatsKHR( vk.physicalDevice, vk.surface, &numFormats, NULL ) );
	if ( !numFormats ) {
		ri.Error( ERR_FATAL, "Vulkan: the window has no surface format" );
	}
	formats = ri.Hunk_AllocateTempMemory( sizeof( *formats ) * numFormats );
	VK_CHECK( qvkGetPhysicalDeviceSurfaceFormatsKHR( vk.physicalDevice, vk.surface, &numFormats, formats ) );

	// HDR output: an HDR format when the surface has one and the window is
	// in HDR (vk_output.c)
	if ( vko.enabled && VK_OutputChooseFormat( formats, numFormats, &chosen ) ) {
		ri.Hunk_FreeTempMemory( formats );
		return chosen;
	}

	// a plain 8-bit UNORM format, like the GL framebuffer (no sRGB encoding)
	chosen.format = VK_FORMAT_UNDEFINED;
	for ( j = 0; j < ARRAY_LEN( preferred ) && chosen.format == VK_FORMAT_UNDEFINED; j++ ) {
		for ( i = 0; i < numFormats; i++ ) {
			if ( formats[i].format == preferred[j] && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
				&& VK_FormatSupportsBlitDst( formats[i].format ) ) {
				chosen = formats[i];
				break;
			}
		}
	}

	// anything we can blit to
	for ( i = 0; i < numFormats && chosen.format == VK_FORMAT_UNDEFINED; i++ ) {
		if ( formats[i].format != VK_FORMAT_UNDEFINED && VK_FormatSupportsBlitDst( formats[i].format ) ) {
			chosen = formats[i];
		}
	}

	ri.Hunk_FreeTempMemory( formats );

	if ( chosen.format == VK_FORMAT_UNDEFINED ) {
		ri.Error( ERR_FATAL, "Vulkan: no swapchain format supports blitting" );
	}
	return chosen;
}

static VkPresentModeKHR VK_ChoosePresentMode( void ) {
	VkPresentModeKHR *modes;
	VkPresentModeKHR wanted[2], chosen;
	uint32_t numModes, numWanted, i, j;

	VK_CHECK( qvkGetPhysicalDeviceSurfacePresentModesKHR( vk.physicalDevice, vk.surface, &numModes, NULL ) );
	modes = ri.Hunk_AllocateTempMemory( sizeof( *modes ) * ( numModes + 1 ) );
	VK_CHECK( qvkGetPhysicalDeviceSurfacePresentModesKHR( vk.physicalDevice, vk.surface, &numModes, modes ) );

	// r_swapInterval: 0 no vsync, > 0 vsync, < 0 adaptive vsync; FIFO is
	// always available. Without vsync immediate, which tears, or with
	// r_fastSync mailbox, which doesn't: every refresh shows the newest
	// finished frame, for the wait for the compositor's latch (see README,
	// Present timing); each is the other's fallback
	numWanted = 0;
	if ( r_swapInterval->integer == 0 ) {
		wanted[numWanted++] = r_fastSync->integer ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
		wanted[numWanted++] = r_fastSync->integer ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_MAILBOX_KHR;
	} else if ( r_swapInterval->integer < 0 ) {
		wanted[numWanted++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	}

	chosen = VK_PRESENT_MODE_FIFO_KHR;
	for ( j = 0; j < numWanted && chosen == VK_PRESENT_MODE_FIFO_KHR; j++ ) {
		for ( i = 0; i < numModes; i++ ) {
			if ( modes[i] == wanted[j] ) {
				chosen = wanted[j];
				break;
			}
		}
	}
	ri.Hunk_FreeTempMemory( modes );

	return chosen;
}

static const char *VK_PresentModeName( VkPresentModeKHR mode ) {
	switch ( mode ) {
	case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
	case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
	case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo relaxed";
	default: return "?";
	}
}

/*
====================================================================

PRESENT FENCES AND RETIRED SWAPCHAINS

A present waits for a semaphore that the frame's submission signals, one
per image of the swapchain (vk.presentSemaphores, created with it). The
semaphore of an image is signaled again only after the image has been
acquired again and its acquire semaphore waited for, which guarantees that
the image's previous present has completed, its wait included. When the
swapchain is replaced (or destroyed while the window is minimized, or at
shutdown) it and its semaphores are retired: no image of it is presented
any more, but the presentation engine may still use them.

With VK_KHR/EXT_swapchain_maintenance1 every present also signals a fence
(vk.presentFenceSlots) once the presentation engine is done with its wait
semaphore; the fences of the presents to one swapchain signal in present
order, so a retired swapchain and its semaphores are destroyed once the
fence of its last present has signaled (checked after every frame's present,
waited for at shutdown). Without the extension they are destroyed when they
are retired: that always follows a vkDeviceWaitIdle after their last
present, and vkQueueWaitIdle waits for the queue operations of presents too
(the semaphore waits; Vulkan specification, vkQueueWaitIdle and
vkQueuePresentKHR).

Nothing here waits between a frame's input and its present: the fence of
the next present is chosen and reset right after a present
(VK_PreparePresentFence, called by vk_end_frame), a slot whose present is
done is reused and one is added when none is, and the present only adds it
to its VkPresentInfoKHR.
====================================================================
*/

#define VK_RETIRED_WAIT_NS		1000000000ull	// at shutdown, at most this long for a swapchain's presents

// the present has completed: the slot can be reused
static qboolean VK_PresentFenceDone( vkPresentFence_t *slot ) {
	if ( slot->pending && qvkGetFenceStatus( vk.device, slot->fence ) == VK_SUCCESS ) {
		slot->pending = qfalse;
	}
	return !slot->pending;
}

/*
===============
VK_TryPreparePresentFence

Chooses and resets the fence of the next present; returns the error of a
call that failed (the submit thread calls it, which mustn't raise errors)
===============
*/
VkResult VK_TryPreparePresentFence( void ) {
	vkPresentFence_t *slot;
	VkResult result;
	int i, s;

	if ( !vk.presentFences || vk.nextPresentFence >= 0 ) {
		return VK_SUCCESS;
	}

	// the slots are taken in turn: the one after the last taken holds the
	// oldest present
	for ( i = 0, s = -1; i < vk.numPresentFences && s < 0; i++ ) {
		if ( VK_PresentFenceDone( &vk.presentFenceSlots[( vk.presentFenceScan + i ) % vk.numPresentFences] ) ) {
			s = ( vk.presentFenceScan + i ) % vk.numPresentFences;
		}
	}

	if ( s < 0 && vk.numPresentFences < VK_PRESENT_FENCES ) {
		VkFenceCreateInfo info;

		slot = &vk.presentFenceSlots[vk.numPresentFences];
		Com_Memset( &info, 0, sizeof( info ) );
		info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		result = qvkCreateFence( vk.device, &info, NULL, &slot->fence );
		if ( result != VK_SUCCESS ) {
			return result;
		}
		s = vk.numPresentFences++;
		slot->pending = qfalse;
		slot->uses = 0;
	} else {
		if ( s < 0 ) {
			// VK_PRESENT_FENCES presents pending, more than any swapchain
			// has images: not seen. Wait for the oldest
			s = vk.presentFenceScan % vk.numPresentFences;
			result = qvkWaitForFences( vk.device, 1, &vk.presentFenceSlots[s].fence, VK_TRUE, UINT64_MAX );
			if ( result != VK_SUCCESS ) {
				return result;
			}
			vk.presentFenceSlots[s].pending = qfalse;
		}
		slot = &vk.presentFenceSlots[s];
		if ( slot->uses ) {
			result = qvkResetFences( vk.device, 1, &slot->fence );
			if ( result != VK_SUCCESS ) {
				return result;
			}
		}
	}

	vk.nextPresentFence = s;
	vk.presentFenceScan = ( s + 1 ) % vk.numPresentFences;
	return VK_SUCCESS;
}

void VK_PreparePresentFence( void ) {
	VK_CHECK( VK_TryPreparePresentFence() );
}

/*
===============
VK_PresentFenceUsed

After a present that queued its operations (VK_SUCCESS, VK_SUBOPTIMAL_KHR
or VK_ERROR_OUT_OF_DATE_KHR): its fence will signal
===============
*/
void VK_PresentFenceUsed( void ) {
	vkPresentFence_t *slot;

	if ( vk.nextPresentFence < 0 ) {
		return;
	}
	slot = &vk.presentFenceSlots[vk.nextPresentFence];
	slot->pending = qtrue;
	slot->uses++;
	vk.lastPresentFence = vk.nextPresentFence;
	vk.lastPresentFenceUse = slot->uses;
	vk.nextPresentFence = -1;
}

/*
===============
VK_RetiredSwapchainDone

The presentation engine is done with the swapchain and its semaphores
===============
*/
static qboolean VK_RetiredSwapchainDone( const vkRetiredSwapchain_t *retired, qboolean wait ) {
	vkPresentFence_t *slot;
	VkResult result;

	// without present fences a vkDeviceWaitIdle came after its last present
	if ( !vk.presentFences || retired->lastFence < 0 ) {
		return qtrue;
	}
	// a slot is only taken again once its fence has signaled
	slot = &vk.presentFenceSlots[retired->lastFence];
	if ( slot->uses != retired->lastFenceUse || VK_PresentFenceDone( slot ) ) {
		return qtrue;
	}
	if ( !wait ) {
		return qfalse;
	}

	result = qvkWaitForFences( vk.device, 1, &slot->fence, VK_TRUE, VK_RETIRED_WAIT_NS );
	if ( result == VK_SUCCESS ) {
		slot->pending = qfalse;
	} else {
		// destroyed anyway (shutdown, or more retired swapchains than
		// VK_MAX_RETIRED_SWAPCHAINS), but say so
		ri.Printf( PRINT_ALL, S_COLOR_YELLOW "WARNING: Vulkan: the last present of a swapchain didn't complete (%s)\n",
			VK_ResultString( result ) );
	}
	return qtrue;
}

/*
===============
VK_DestroyRetiredSwapchains

Destroys the retired swapchains whose presents are done, all of them with
wait (at shutdown)
===============
*/
void VK_DestroyRetiredSwapchains( qboolean wait ) {
	int i, n;
	uint32_t j;

	for ( i = n = 0; i < vk.numRetiredSwapchains; i++ ) {
		vkRetiredSwapchain_t *retired = &vk.retiredSwapchains[i];

		if ( !VK_RetiredSwapchainDone( retired, wait ) ) {
			vk.retiredSwapchains[n++] = *retired;
			continue;
		}
		qvkDestroySwapchainKHR( vk.device, retired->swapchain, NULL );
		for ( j = 0; j < retired->numSemaphores; j++ ) {
			qvkDestroySemaphore( vk.device, retired->semaphores[j], NULL );
		}
	}
	vk.numRetiredSwapchains = n;
}

/*
===============
VK_RetireSwapchain

The swapchain is replaced or destroyed, and nothing will be presented to
it; the caller waited for the device after its last present and released
its held image
===============
*/
static void VK_RetireSwapchain( VkSwapchainKHR swapchain ) {
	vkRetiredSwapchain_t *retired;

	if ( vk.numRetiredSwapchains == VK_MAX_RETIRED_SWAPCHAINS ) {
		// not seen: the oldest ones are done after a frame or two
		VK_DestroyRetiredSwapchains( qtrue );
	}

	retired = &vk.retiredSwapchains[vk.numRetiredSwapchains++];
	retired->swapchain = swapchain;
	retired->numSemaphores = vk.numPresentSemaphores;
	Com_Memcpy( retired->semaphores, vk.presentSemaphores, sizeof( retired->semaphores ) );
	retired->lastFence = vk.lastPresentFence;
	retired->lastFenceUse = vk.lastPresentFenceUse;

	Com_Memset( vk.presentSemaphores, 0, sizeof( vk.presentSemaphores ) );
	vk.numPresentSemaphores = 0;
	vk.lastPresentFence = -1;

	// usually done already: the device was waited for
	VK_DestroyRetiredSwapchains( qfalse );
}

// the present semaphores of a new swapchain, one per image
static void VK_CreatePresentSemaphores( void ) {
	VkSemaphoreCreateInfo info;
	uint32_t i;

	Com_Memset( &info, 0, sizeof( info ) );
	info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for ( i = 0; i < vk.swapchainImageCount; i++ ) {
		VK_CHECK( qvkCreateSemaphore( vk.device, &info, NULL, &vk.presentSemaphores[i] ) );
	}
	vk.numPresentSemaphores = vk.swapchainImageCount;
	vk.lastPresentFence = -1;
}

/*
===============
VK_DestroyPresentFences

At shutdown, once every swapchain is gone
===============
*/
void VK_DestroyPresentFences( void ) {
	int i;

	for ( i = 0; i < vk.numPresentFences; i++ ) {
		qvkDestroyFence( vk.device, vk.presentFenceSlots[i].fence, NULL );
	}
	Com_Memset( vk.presentFenceSlots, 0, sizeof( vk.presentFenceSlots ) );
	vk.numPresentFences = 0;
	vk.nextPresentFence = -1;
	vk.presentFenceScan = 0;
}

/*
===============
VK_CreateSwapchain

Creates the swapchain for the current window size, replacing the previous
one. Leaves vk.swapchain NULL when the window has no area (minimized).
===============
*/
void VK_CreateSwapchain( void ) {
	VkSurfaceCapabilitiesKHR caps;
	VkSurfaceFormatKHR format;
	VkSwapchainCreateInfoKHR createInfo;
	VkSwapchainKHR oldSwapchain;
	VkExtent2D extent;
	uint32_t imageCount;
	int width, height;

	// the submit thread acquires and presents: done with every frame
	// queued before the swapchain changes
	VK_DrainSubmitThread();

	vk.swapchainOutOfDate = qfalse;

	VK_CHECK( qvkGetPhysicalDeviceSurfaceCapabilitiesKHR( vk.physicalDevice, vk.surface, &caps ) );

	SDL_GetWindowSizeInPixels( SDL_window, &width, &height );
	vk.swapchainDrawableWidth = width;
	vk.swapchainDrawableHeight = height;
	if ( caps.currentExtent.width != 0xFFFFFFFF ) {
		extent = caps.currentExtent;
	} else {
		extent.width = Com_Clamp( caps.minImageExtent.width, caps.maxImageExtent.width, width );
		extent.height = Com_Clamp( caps.minImageExtent.height, caps.maxImageExtent.height, height );
	}

	oldSwapchain = vk.swapchain;

	if ( !extent.width || !extent.height ) {
		VK_DestroySwapchain();
		return;
	}

	if ( vko.enabled ) {
		// HDR output: drawn by the output pass
		if ( !( caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ) ) {
			ri.Error( ERR_FATAL, "Vulkan: swapchain images can't be color attachments (r_hdrOutput)" );
		}
		vk.swapchainUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	} else {
		if ( !( caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT ) ) {
			ri.Error( ERR_FATAL, "Vulkan: swapchain images can't be transfer destinations" );
		}
		// drawn to directly and read back when possible, blitted to otherwise
		vk.swapchainUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if ( ( caps.supportedUsageFlags & ( VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT ) )
			== ( VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT ) ) {
			vk.swapchainUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
	}

	format = VK_ChooseSurfaceFormat();
	vk.presentMode = VK_ChoosePresentMode();

	// one more image than the minimum so acquiring doesn't wait on the
	// presentation engine (with mailbox it still does at times: see README,
	// Present timing)
	imageCount = caps.minImageCount + 1;
	if ( vk.submitThread ) {
		// the submit thread acquires (vk_submit.c): five images without
		// vsync, so that one is free whenever it acquires even while the
		// compositor holds some (a fifth adds no lag: every present is shown
		// or replaced at once); with vsync three, the fewest that let it
		// acquire while one image is shown and one waits for the display
		// (FIFO keeps every image presented until the display takes it, so
		// more images would be more frames queued, more lag)
		imageCount = vk.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR || vk.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? 5 : 3;
		if ( imageCount < caps.minImageCount ) {
			imageCount = caps.minImageCount;
		}
	}
	if ( caps.maxImageCount && imageCount > caps.maxImageCount ) {
		imageCount = caps.maxImageCount;
	}
	if ( imageCount > VK_MAX_SWAPCHAIN_IMAGES ) {
		imageCount = VK_MAX_SWAPCHAIN_IMAGES;
	}

	Com_Memset( &createInfo, 0, sizeof( createInfo ) );
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = vk.surface;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = format.format;
	createInfo.imageColorSpace = format.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = vk.swapchainUsage;
	createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	createInfo.preTransform = caps.currentTransform;
	if ( caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ) {
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	} else if ( caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR ) {
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	} else if ( caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR ) {
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;	// alpha is always 1
	} else {
		createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
	}
	createInfo.presentMode = vk.presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = oldSwapchain;

	// nothing may use the old swapchain while it's replaced
	if ( oldSwapchain ) {
		VK_ReleaseHeldImage();
		VK_CHECK( qvkDeviceWaitIdle( vk.device ) );
		VK_DestroySwapchainTargets();
	}

	VK_CHECK( qvkCreateSwapchainKHR( vk.device, &createInfo, NULL, &vk.swapchain ) );
	vk.stats.swapchains++;

	// vsync's pacing starts again with two frames queued (vk.c,
	// VK_WaitForPresentShown); the ids of this swapchain's presents start here
	Com_Memset( &vk.pacing, 0, sizeof( vk.pacing ) );
	vk.pacing.firstPresentId = vk.lastPresentId + 1;
	vk.pacing.depth = 2;

	// the old swapchain and its present semaphores go once its presents
	// are done
	if ( oldSwapchain ) {
		VK_RetireSwapchain( oldSwapchain );
	}

	vk.swapchainFormat = format.format;
	vk.swapchainExtent = extent;

	VK_CHECK( qvkGetSwapchainImagesKHR( vk.device, vk.swapchain, &vk.swapchainImageCount, NULL ) );
	if ( vk.swapchainImageCount > VK_MAX_SWAPCHAIN_IMAGES ) {
		ri.Error( ERR_FATAL, "Vulkan: too many swapchain images (%u)", vk.swapchainImageCount );
	}
	VK_CHECK( qvkGetSwapchainImagesKHR( vk.device, vk.swapchain, &vk.swapchainImageCount, vk.swapchainImages ) );
	VK_CreatePresentSemaphores();
	if ( vko.enabled ) {
		VK_OutputSwapchainCreated();
	}

	// views and framebuffers, once the render pass exists
	VK_CreateSwapchainTargets();

	ri.Printf( PRINT_DEVELOPER, "Vulkan swapchain: %ux%u, %u images, %s\n", extent.width, extent.height,
		vk.swapchainImageCount, VK_PresentModeName( vk.presentMode ) );
}

/*
===============
VK_DestroySwapchain

The window has no area (minimized), or shutdown: the swapchain is retired
like a replaced one; vk_shutdown then waits for it (VK_DestroyRetiredSwapchains)
===============
*/
void VK_DestroySwapchain( void ) {
	if ( vk.swapchain ) {
		VK_DrainSubmitThread();
		VK_ReleaseHeldImage();
		// no VK_CHECK: also called by vk_shutdown, which can follow a lost
		// device, and a second fatal error would skip the window shutdown
		qvkDeviceWaitIdle( vk.device );
		VK_DestroySwapchainTargets();
		VK_RetireSwapchain( vk.swapchain );
		vk.swapchain = VK_NULL_HANDLE;
	}
	vk.swapchainImageCount = 0;
	vk.swapchainExtent.width = vk.swapchainExtent.height = 0;
}

/*
===============
VK_SwapchainPresentModeName
===============
*/
const char *VK_SwapchainPresentModeName( void ) {
	return VK_PresentModeName( vk.presentMode );
}

/*
===============
VK_GetDrawableSize
===============
*/
void VK_GetDrawableSize( int *width, int *height ) {
	SDL_GetWindowSizeInPixels( SDL_window, width, height );
}
