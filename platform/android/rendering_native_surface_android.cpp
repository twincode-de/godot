/**************************************************************************/
/*  rendering_native_surface_android.cpp                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "rendering_native_surface_android.h"
#include <android/native_window.h>

#include "modules/regex/regex.h"

#if defined(VULKAN_ENABLED)
#include "rendering_context_driver_vulkan_android.h"
#endif

#if defined(GLES3_ENABLED)
#include <vector>
#include "core/templates/hash_map.h"
#include "servers/rendering/gl_manager.h"
#include "servers/rendering_server.h"
#include "drivers/egl/gl_manager_embedded_angle.h"
#include <android/native_window.h>
#include <EGL/egl.h>
#if defined(ANGLE_ENABLED)
#include <EGL/eglext_angle.h>
#endif
#include <GLES3/gl3.h>
#include <dlfcn.h>

#define GL_ERR(expr) { expr; GLenum err = glGetError(); if (err) { print_line(vformat("%s:%s: %x error", __FUNCTION__, #expr, err)); } }

#ifndef EGL_KHR_platform_android
#define EGL_KHR_platform_android 1
#define EGL_PLATFORM_ANDROID_KHR          0x3141
#endif /* EGL_KHR_platform_android */

struct WindowData {
	EGLSurface surface = EGL_NO_SURFACE;
	ANativeWindow *window = nullptr;
	uint32_t width;
	uint32_t height;
};

class GLManagerAndroid : public EGLManager {
private:
	HashMap<DisplayServer::WindowID, Size2i> window_sizes;

	virtual const char *_get_platform_extension_name() const override;
	virtual EGLenum _get_platform_extension_enum() const override;
	virtual EGLenum _get_platform_api_enum() const override;
	virtual Vector<EGLAttrib> _get_platform_display_attributes() const override;
	virtual Vector<EGLint> _get_platform_context_attribs() const override;

public:
	Error window_create(DisplayServer::WindowID p_window_id, Ref<RenderingNativeSurface> p_native_surface, int p_width, int p_height) override;
	void window_resize(DisplayServer::WindowID p_window_id, int p_width, int p_height) override;
	void window_destroy(DisplayServer::WindowID p_window_id) override;
	Size2i window_get_size(DisplayServer::WindowID p_window_id) const override;
	bool validate_driver() const override;

	GLManagerAndroid() {}
	~GLManagerAndroid() {}
};

class GLManagerANGLE_Android : public GLManagerANGLE_Embedded {
private:
	EGLAttrib active_backend_type = EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE;
	virtual Vector<EGLAttrib> _get_platform_display_attributes() const override;

public:
	Error initialize(void *p_native_display = nullptr) override;
	Error window_create(DisplayServer::WindowID p_window_id, Ref<RenderingNativeSurface> p_native_surface, int p_width, int p_height) override;
	bool validate_driver() const override;

	GLManagerANGLE_Android() {}
	~GLManagerANGLE_Android() {}
};

static Size2i get_android_surface_size(const Ref<RenderingNativeSurface> &p_native_surface, int p_width, int p_height) {
	Size2i size(p_width, p_height);

	Ref<RenderingNativeSurfaceAndroid> android_surface = p_native_surface;
	if (android_surface.is_valid()) {
		size.width = android_surface->get_width();
		size.height = android_surface->get_height();

		if ((size.width <= 0 || size.height <= 0) && android_surface->get_window() != nullptr) {
			size.width = ANativeWindow_getWidth(android_surface->get_window());
			size.height = ANativeWindow_getHeight(android_surface->get_window());
		}
	}

	return size;
}

static bool validate_android_driver(const char *p_library_name) {
	void *handle = dlopen(p_library_name, RTLD_LOCAL);
	if (handle == nullptr) {
		CRASH_NOW_MSG(vformat("Unable to open %s", p_library_name));
	}
	PFNGLGETSTRINGPROC get_string_proc = (PFNGLGETSTRINGPROC)dlsym(handle, "glGetString");
	ERR_FAIL_COND_V_MSG(get_string_proc == nullptr, false, "Unable to load glGetString symbol");

	const String rendering_device_name = String::utf8((const char *)get_string_proc(GL_RENDERER));
	const String rendering_device_vendor = String::utf8((const char *)get_string_proc(GL_VENDOR));
	print_line(vformat("Device name: %s", rendering_device_name));
	print_line(vformat("Vendor: %s", rendering_device_vendor));
	dlclose(handle);
	if (rendering_device_name.contains("PowerVR") || rendering_device_vendor.contains("Imagination")) {
		print_line("Detected Imagination GPU");
	}
	return true;
}

static Vector<EGLAttrib> get_android_angle_display_attributes_for_backend_type(EGLAttrib p_backend_type) {
	Vector<EGLAttrib> ret;
	ret.push_back(EGL_PLATFORM_ANGLE_TYPE_ANGLE);
	ret.push_back(p_backend_type);
	if (p_backend_type == EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE) {
		ret.push_back(EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE);
		ret.push_back(EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE);
	}
	ret.push_back(EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE);
	ret.push_back(EGL_PLATFORM_ANDROID_KHR);
	ret.push_back(EGL_NONE);
	return ret;
}

const char *GLManagerAndroid::_get_platform_extension_name() const {
	return "EGL_KHR_platform_android";
}

EGLenum GLManagerAndroid::_get_platform_extension_enum() const {
	return EGL_PLATFORM_ANDROID_KHR;
}

Vector<EGLAttrib> GLManagerAndroid::_get_platform_display_attributes() const {
	Vector<EGLAttrib> ret;
	return ret;
}

EGLenum GLManagerAndroid::_get_platform_api_enum() const {
	return EGL_OPENGL_ES_API;
}

Vector<EGLint> GLManagerAndroid::_get_platform_context_attribs() const {
	Vector<EGLint> ret;
	ret.push_back(EGL_CONTEXT_CLIENT_VERSION);
	ret.push_back(3);
	ret.push_back(EGL_NONE);

	return ret;
}

bool GLManagerAndroid::validate_driver() const {
	return validate_android_driver("libGLESv3.so");
}

Error GLManagerAndroid::window_create(DisplayServer::WindowID p_window_id, Ref<RenderingNativeSurface> p_native_surface, int p_width, int p_height) {
	Size2i size = get_android_surface_size(p_native_surface, p_width, p_height);
	window_sizes.insert(p_window_id, size);
	return EGLManager::window_create(p_window_id, p_native_surface, size.width, size.height);
}

void GLManagerAndroid::window_resize(DisplayServer::WindowID p_window_id, int p_width, int p_height) {
	window_sizes.insert(p_window_id, Size2i(p_width, p_height));
}

void GLManagerAndroid::window_destroy(DisplayServer::WindowID p_window_id) {
	window_sizes.erase(p_window_id);
	EGLManager::window_destroy(p_window_id);
}

Size2i GLManagerAndroid::window_get_size(DisplayServer::WindowID p_window_id) const {
	const Size2i *size = window_sizes.getptr(p_window_id);
	if (size != nullptr) {
		return *size;
	}
	return Size2i();
}

Error GLManagerANGLE_Android::window_create(DisplayServer::WindowID p_window_id, Ref<RenderingNativeSurface> p_native_surface, int p_width, int p_height) {
	Size2i size = get_android_surface_size(p_native_surface, p_width, p_height);
	return GLManagerANGLE_Embedded::window_create(p_window_id, p_native_surface, size.width, size.height);
}

Vector<EGLAttrib> GLManagerANGLE_Android::_get_platform_display_attributes() const {
	return get_android_angle_display_attributes_for_backend_type(active_backend_type);
}

Error GLManagerANGLE_Android::initialize(void *p_native_display) {
#if defined(GLAD_ENABLED) && !defined(EGL_STATIC)
	void *handle = dlopen("libEGL_angle.so", RTLD_NOW | RTLD_LOCAL);
	ERR_FAIL_NULL_V_MSG(handle, ERR_UNAVAILABLE, vformat("Can't load ANGLE EGL dynamic library: %s", dlerror()));

	PFNEGLGETPROCADDRESSPROC get_proc_address = (PFNEGLGETPROCADDRESSPROC)dlsym(handle, "eglGetProcAddress");
	ERR_FAIL_NULL_V_MSG(get_proc_address, ERR_UNAVAILABLE, "Can't load eglGetProcAddress from ANGLE EGL library.");

	PFNEGLGETPLATFORMDISPLAYPROC get_platform_display = (PFNEGLGETPLATFORMDISPLAYPROC)dlsym(handle, "eglGetPlatformDisplay");
	if (get_platform_display == nullptr) {
		get_platform_display = (PFNEGLGETPLATFORMDISPLAYPROC)get_proc_address("eglGetPlatformDisplay");
	}

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext = (PFNEGLGETPLATFORMDISPLAYEXTPROC)dlsym(handle, "eglGetPlatformDisplayEXT");
	if (get_platform_display_ext == nullptr) {
		get_platform_display_ext = (PFNEGLGETPLATFORMDISPLAYEXTPROC)get_proc_address("eglGetPlatformDisplayEXT");
	}
	ERR_FAIL_COND_V_MSG(get_platform_display == nullptr && get_platform_display_ext == nullptr, ERR_UNAVAILABLE, "ANGLE EGL platform display entry points are unavailable.");

	PFNEGLINITIALIZEPROC initialize_proc = (PFNEGLINITIALIZEPROC)dlsym(handle, "eglInitialize");
	PFNEGLTERMINATEPROC terminate_proc = (PFNEGLTERMINATEPROC)dlsym(handle, "eglTerminate");
	ERR_FAIL_NULL_V_MSG(initialize_proc, ERR_UNAVAILABLE, "Can't load eglInitialize from ANGLE EGL library.");

	EGLNativeDisplayType native_display = EGL_DEFAULT_DISPLAY;

	Vector<EGLAttrib> backend_attempts;
	backend_attempts.push_back(EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE);
	backend_attempts.push_back(EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE);

	EGLDisplay tmp_display = EGL_NO_DISPLAY;

	for (int i = 0; i < backend_attempts.size(); i++) {
		const EGLAttrib backend_type = backend_attempts[i];
		Vector<EGLAttrib> attribs = get_android_angle_display_attributes_for_backend_type(backend_type);
		Vector<EGLint> attribs_ext;
		for (const EGLAttrib &attrib : attribs) {
			attribs_ext.push_back((EGLint)attrib);
		}

		if (get_platform_display_ext != nullptr) {
			tmp_display = get_platform_display_ext(EGL_PLATFORM_ANGLE_ANGLE, native_display, attribs_ext.ptr());
		} else {
			tmp_display = get_platform_display(EGL_PLATFORM_ANGLE_ANGLE, native_display, attribs.ptr());
		}
		if (tmp_display == EGL_NO_DISPLAY) {
			continue;
		}

		if (initialize_proc(tmp_display, nullptr, nullptr)) {
			active_backend_type = backend_type;
			break;
		}

		if (terminate_proc != nullptr) {
			terminate_proc(tmp_display);
		}
		tmp_display = EGL_NO_DISPLAY;
	}

	ERR_FAIL_COND_V_MSG(tmp_display == EGL_NO_DISPLAY, ERR_UNAVAILABLE, "Can't initialize the initial ANGLE EGL display.");

	int version = gladLoaderLoadEGL(tmp_display);
	if (terminate_proc != nullptr) {
		terminate_proc(tmp_display);
	}
	if (!version) {
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "Can't load ANGLE EGL dynamic library.");
	}

	int major = GLAD_VERSION_MAJOR(version);
	int minor = GLAD_VERSION_MINOR(version);

	ERR_FAIL_COND_V_MSG(!GLAD_EGL_VERSION_1_4, ERR_UNAVAILABLE, vformat("EGL version is too old! %d.%d < 1.4", major, minor));
#endif

	return OK;
}

bool GLManagerANGLE_Android::validate_driver() const {
	// ANGLE's GLES entry points can crash when probed directly via dlsym/glGetString
	// during driver validation, even though context creation succeeded.
	return true;
}

#endif // GLES3_ENABLED

void RenderingNativeSurfaceAndroid::_bind_methods() {
	ClassDB::bind_static_method("RenderingNativeSurfaceAndroid", D_METHOD("create", "window", "width", "height"), &RenderingNativeSurfaceAndroid::create_api);
	ClassDB::bind_method(D_METHOD("get_window"), &RenderingNativeSurfaceAndroid::get_window_api);
	ClassDB::bind_method(D_METHOD("get_width"), &RenderingNativeSurfaceAndroid::get_width);
	ClassDB::bind_method(D_METHOD("get_height"), &RenderingNativeSurfaceAndroid::get_height);
}

Ref<RenderingNativeSurfaceAndroid> RenderingNativeSurfaceAndroid::create_api(uint64_t p_window, uint32_t p_width, uint32_t p_height) {
	return RenderingNativeSurfaceAndroid::create((ANativeWindow *)p_window, p_width, p_height);
}

Ref<RenderingNativeSurfaceAndroid> RenderingNativeSurfaceAndroid::create(ANativeWindow *p_window, uint32_t p_width, uint32_t p_height) {
	Ref<RenderingNativeSurfaceAndroid> result = memnew(RenderingNativeSurfaceAndroid);
	result->window = p_window;
	result->width = p_width;
	result->height = p_height;
	return result;
}

RenderingContextDriver *RenderingNativeSurfaceAndroid::create_rendering_context(const String &p_driver_name) {
#if defined(VULKAN_ENABLED)
	if (p_driver_name == "vulkan") {
		return memnew(RenderingContextDriverVulkanAndroid);
	}
#endif
	return nullptr;
}

GLManager *RenderingNativeSurfaceAndroid::create_gl_manager(const String &p_driver_name) {
#if defined(GLES3_ENABLED)
	if (p_driver_name == "opengl3") {
		#ifdef GLAD_ENABLED
			static const char *EGL_NAMES[] = {"libEGL.so"};
			static const char *GL_NAMES[] = {"libGLESv3.so"};
			gladSetupEGL(1, EGL_NAMES);
			gladSetupGLES2(1, GL_NAMES);
        #endif
        return memnew(GLManagerAndroid);
	}
	#if defined(ANGLE_ENABLED)
	if (p_driver_name == "opengl3_angle") {
		setenv("ANGLE_FEATURE_OVERRIDES_DISABLED", "supportsSwapchainMaintenance1", 1);
		#ifdef GLAD_ENABLED
			static const char *EGL_NAMES[] = {"libEGL_angle.so"};
			static const char *GL_NAMES[] = {"libGLESv2_angle.so"};
			gladSetupEGL(1, EGL_NAMES);
			gladSetupGLES2(1, GL_NAMES);
		#endif
		return memnew(GLManagerANGLE_Android);
	}
	#endif
#endif
	return nullptr;
}

void *RenderingNativeSurfaceAndroid::get_native_id() const {
	return (void *)window;
}


RenderingNativeSurfaceAndroid::RenderingNativeSurfaceAndroid() {
	// Does nothing.
}

RenderingNativeSurfaceAndroid::~RenderingNativeSurfaceAndroid() {
	// Does nothing.
}
