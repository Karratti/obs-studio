#include <opts-parser.h>
#include <obs-module.h>
#include <obs-avc.h>

#include "obs-ffmpeg-config.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <map>

#include "external/AMF/include/components/VideoEncoderHEVC.h"
#include "external/AMF/include/components/VideoEncoderVCE.h"
#include "external/AMF/include/core/Factory.h"
#include "external/AMF/include/core/Trace.h"

#include <dxgi.h>
#include <d3d11.h>
#include <d3d11_1.h>

#include <util/windows/HRError.hpp>
#include <util/windows/ComPtr.hpp>
#include <util/platform.h>
#include <util/util.hpp>
#include <util/pipe.h>
#include <util/dstr.h>

using namespace amf;

/* ========================================================================= */
/* Junk                                                                      */

#define do_log(level, format, ...)                 \
	blog(level, "[texture-amf: '%s'] " format, \
	     obs_encoder_get_name(enc->encoder), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR, format, ##__VA_ARGS__)
#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

struct amf_error {
	const char *str;
	AMF_RESULT res;

	inline amf_error(const char *str, AMF_RESULT res) : str(str), res(res)
	{
	}
};

struct handle_tex {
	uint32_t handle;
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<IDXGIKeyedMutex> km;
};

struct adapter_caps {
	bool is_amd = false;
	bool supports_avc = false;
	bool supports_hevc = false;
};

/* ------------------------------------------------------------------------- */

static std::map<uint32_t, adapter_caps> caps;
static bool h264_supported = false;
static AMFFactory *amf_factory = nullptr;
static AMFTrace *amf_trace = nullptr;
static HMODULE amf_module = nullptr;
static uint64_t amf_version = 0;

/* ========================================================================= */
/* Main Implementation                                                       */

enum class amf_codec_type {
	AVC,
	HEVC,
};

struct amf_data {
	obs_encoder_t *encoder;

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11Texture2D> texture;

	amf_codec_type codec;
	AMFContextPtr amf_context;
	AMFComponentPtr amf_encoder;
	AMFBufferPtr packet_data;
	AMFRate amf_frame_rate;
	AMF_VIDEO_CONVERTER_COLOR_PROFILE_ENUM amf_color_profile;
	AMF_COLOR_TRANSFER_CHARACTERISTIC_ENUM amf_characteristic;
	AMF_COLOR_PRIMARIES_ENUM amf_primaries;
	AMF_SURFACE_FORMAT amf_format;

	std::vector<handle_tex> input_textures;
	std::deque<int64_t> dts_list;

	uint32_t cx;
	uint32_t cy;
	int fps_num;
	int fps_den;
	bool full_range;

	AMFBufferPtr header;
};

/* ------------------------------------------------------------------------- */
/* More garbage                                                              */

template<typename T>
static void set_amf_property(amf_data *enc, const wchar_t *name, const T &value)
{
	AMF_RESULT res = enc->amf_encoder->SetProperty(name, value);
	if (res != AMF_OK)
		error("Failed to set property '%ls': %ls", name,
		      amf_trace->GetResultText(res));
}

#define set_avc_property(enc, name, value) \
	set_amf_property(enc, AMF_VIDEO_ENCODER_##name, value)
#define set_hevc_property(enc, name, value) \
	set_amf_property(enc, AMF_VIDEO_ENCODER_HEVC_##name, value)

#define get_opt_name(name)                                              \
	((enc->codec == amf_codec_type::AVC) ? AMF_VIDEO_ENCODER_##name \
					     : AMF_VIDEO_ENCODER_HEVC_##name)
#define set_opt(name, value) set_amf_property(enc, get_opt_name(name), value)
#define set_avc_opt(name, value) set_avc_property(enc, name, value)
#define set_hevc_opt(name, value) set_hevc_property(enc, name, value)
#define set_enum_opt(name, value) \
	set_amf_property(enc, get_opt_name(name), get_opt_name(name##_##value))
#define set_avc_enum(name, value) \
	set_avc_property(enc, name, AMF_VIDEO_ENCODER_##name##_##value)
#define set_hevc_enum(name, value) \
	set_hevc_property(enc, name, AMF_VIDEO_ENCODER_HEVC_##name##_##value)

/* ------------------------------------------------------------------------- */
/* Implementation                                                            */

static HMODULE get_lib(amf_data *enc, const char *lib)
{
	HMODULE mod = GetModuleHandleA(lib);
	if (mod)
		return mod;

	mod = LoadLibraryA(lib);
	if (!mod)
		error("Failed to load %s", lib);
	return mod;
}

#define AMD_VENDOR_ID 0x1002

typedef HRESULT(WINAPI *CREATEDXGIFACTORY1PROC)(REFIID, void **);

static bool amf_init_d3d11(amf_data *enc)
try {
	HMODULE dxgi = get_lib(enc, "DXGI.dll");
	HMODULE d3d11 = get_lib(enc, "D3D11.dll");
	CREATEDXGIFACTORY1PROC create_dxgi;
	PFN_D3D11_CREATE_DEVICE create_device;
	ComPtr<IDXGIFactory> factory;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc;
	HRESULT hr;

	if (!dxgi || !d3d11)
		throw "Couldn't get D3D11/DXGI libraries? "
		      "That definitely shouldn't be possible.";

	create_dxgi = (CREATEDXGIFACTORY1PROC)GetProcAddress(
		dxgi, "CreateDXGIFactory1");
	create_device = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(
		d3d11, "D3D11CreateDevice");

	if (!create_dxgi || !create_device)
		throw "Failed to load D3D11/DXGI procedures";

	hr = create_dxgi(__uuidof(IDXGIFactory2), (void **)&factory);
	if (FAILED(hr))
		throw HRError("CreateDXGIFactory1 failed", hr);

	obs_video_info ovi;
	obs_get_video_info(&ovi);

	hr = factory->EnumAdapters(ovi.adapter, &adapter);
	if (FAILED(hr))
		throw HRError("EnumAdapters failed", hr);

	adapter->GetDesc(&desc);
	if (desc.VendorId != AMD_VENDOR_ID)
		throw "Seems somehow AMF is trying to initialize "
		      "on a non-AMD adapter";

	hr = create_device(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
			   nullptr, 0, D3D11_SDK_VERSION, &device, nullptr,
			   &context);
	if (FAILED(hr))
		throw HRError("D3D11CreateDevice failed", hr);

	enc->device = device;
	enc->context = context;
	return true;

} catch (const HRError &err) {
	error("%s: %s: 0x%lX", __FUNCTION__, err.str, err.hr);
	return false;

} catch (const char *err) {
	error("%s: %s", __FUNCTION__, err);
	return false;
}

static inline void create_texture(amf_data *enc, ID3D11Texture2D *from)
{
	ID3D11Device *device = enc->device;
	HRESULT hr;

	D3D11_TEXTURE2D_DESC desc;
	from->GetDesc(&desc);
	desc.BindFlags = D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = 0;

	hr = device->CreateTexture2D(&desc, nullptr, &enc->texture);
	if (FAILED(hr))
		throw HRError("Failed to create texture", hr);
}

static void get_tex_from_handle(amf_data *enc, uint32_t handle,
				IDXGIKeyedMutex **km_out,
				ID3D11Texture2D **tex_out)
{
	ID3D11Device *device = enc->device;
	ComPtr<ID3D11Texture2D> tex;
	HRESULT hr;

	for (size_t i = 0; i < enc->input_textures.size(); i++) {
		struct handle_tex &ht = enc->input_textures[i];
		if (ht.handle == handle) {
			ht.km.CopyTo(km_out);
			ht.tex.CopyTo(tex_out);
			return;
		}
	}

	hr = device->OpenSharedResource((HANDLE)(uintptr_t)handle,
					__uuidof(ID3D11Resource),
					(void **)&tex);
	if (FAILED(hr))
		throw HRError("OpenSharedResource failed", hr);

	ComQIPtr<IDXGIKeyedMutex> km(tex);
	if (!km)
		throw "QueryInterface(IDXGIKeyedMutex) failed";

	tex->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);

	struct handle_tex new_ht = {handle, tex, km};
	enc->input_textures.push_back(std::move(new_ht));

	*km_out = km.Detach();
	*tex_out = tex.Detach();
}

static inline int64_t convert_to_amf_ts(amf_data *enc, int64_t ts)
{
	constexpr int64_t amf_timebase = AMF_SECOND;
	return ts * amf_timebase / (int64_t)enc->fps_den;
}

static inline int64_t convert_to_obs_ts(amf_data *enc, int64_t ts)
{
	constexpr int64_t amf_timebase = AMF_SECOND;
	return ts * (int64_t)enc->fps_den / amf_timebase;
}

static void convert_to_encoder_packet(amf_data *enc, AMFDataPtr &data,
				      encoder_packet *packet)
{
	if (!data)
		return;

	enc->packet_data = AMFBufferPtr(data);
	data->GetProperty(L"PTS", &packet->pts);

	bool hevc = enc->codec == amf_codec_type::HEVC;
	const wchar_t *get_output_type =
		hevc ? AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE
		     : AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE;

	uint64_t type;
	data->GetProperty(get_output_type, &type);

	switch (type) {
	case AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR:
		packet->priority = OBS_NAL_PRIORITY_HIGHEST;
		break;
	case AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_I:
		packet->priority = OBS_NAL_PRIORITY_HIGH;
		break;
	case AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_P:
		packet->priority = OBS_NAL_PRIORITY_LOW;
		break;
	case AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_B:
		packet->priority = OBS_NAL_PRIORITY_DISPOSABLE;
		break;
	}

	packet->data = (uint8_t *)enc->packet_data->GetNative();
	packet->size = enc->packet_data->GetSize();
	packet->type = OBS_ENCODER_VIDEO;
	packet->dts = convert_to_obs_ts(enc, data->GetPts());
	packet->keyframe = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
}

static bool amf_encode_tex(void *data, uint32_t handle, int64_t pts,
			   uint64_t lock_key, uint64_t *next_key,
			   encoder_packet *packet, bool *received_packet)
try {
	amf_data *enc = (amf_data *)data;
	ID3D11DeviceContext *context = enc->context;
	ComPtr<ID3D11Texture2D> output_tex;
	ComPtr<ID3D11Texture2D> input_tex;
	ComPtr<IDXGIKeyedMutex> km;
	AMFSurfacePtr amf_surf;
	AMFDataPtr amf_out;
	AMF_RESULT res;

	if (handle == GS_INVALID_HANDLE) {
		*next_key = lock_key;
		throw "Encode failed: bad texture handle";
	}

	get_tex_from_handle(enc, handle, &km, &input_tex);
	if (!enc->texture)
		create_texture(enc, input_tex);
	output_tex = enc->texture;

	/* ------------------------------------ */
	/* copy to output tex                   */

	km->AcquireSync(lock_key, INFINITE);
	context->CopyResource((ID3D11Resource *)output_tex.Get(),
			      (ID3D11Resource *)input_tex.Get());
	context->Flush();
	km->ReleaseSync(*next_key);

	/* ------------------------------------ */
	/* map output tex to amf surface        */

	/* XXX: does this really need to be recreated every frame? */

	res = enc->amf_context->CreateSurfaceFromDX11Native(output_tex,
							    &amf_surf, nullptr);
	if (res != AMF_OK)
		throw amf_error("CreateSurfaceFromDX11Native failed", res);

	int64_t last_ts = convert_to_amf_ts(enc, pts - 1);
	int64_t cur_ts = convert_to_amf_ts(enc, pts);

	amf_surf->SetPts(cur_ts);
	amf_surf->SetProperty(L"PTS", pts);
	amf_surf->SetDuration(cur_ts - last_ts);

	/* ------------------------------------ */
	/* do actual encode                     */

	res = enc->amf_encoder->SubmitInput(amf_surf);
	if (res != AMF_OK)
		throw amf_error("SubmitInput failed", res);

	for (;;) {
		res = enc->amf_encoder->QueryOutput(&amf_out);
		if (res == AMF_OK)
			break;

		switch (res) {
		case AMF_NEED_MORE_INPUT:
			*received_packet = false;
			return true;
		case AMF_REPEAT:
			break;
		default:
			throw amf_error("QueryOutput failed", res);
		}

		os_sleep_ms(1);
	}

	*received_packet = true;
	convert_to_encoder_packet(enc, amf_out, packet);
	return true;

} catch (const char *err) {
	amf_data *enc = (amf_data *)data;
	error("%s: %s", __FUNCTION__, err);
	return false;

} catch (const amf_error &err) {
	amf_data *enc = (amf_data *)data;
	error("%s: %s: %s", __FUNCTION__, err.str,
	      amf_trace->GetResultText(err.res));
	*received_packet = false;
	return false;

} catch (const HRError &err) {
	amf_data *enc = (amf_data *)data;
	error("%s: %s: 0x%lX", __FUNCTION__, err.str, err.hr);
	*received_packet = false;
	return false;
}

static bool amf_extra_data(void *data, uint8_t **header, size_t *size)
{
	amf_data *enc = (amf_data *)data;
	if (!enc->header)
		return false;

	*header = (uint8_t *)enc->header->GetNative();
	*size = enc->header->GetSize();
	return true;
}

static bool amf_create_encoder(amf_data *enc)
try {
	AMF_RESULT res;

	/* ------------------------------------ */
	/* get video info                       */

	struct obs_video_info ovi;
	obs_get_video_info(&ovi);

	enc->cx = obs_encoder_get_width(enc->encoder);
	enc->cy = obs_encoder_get_height(enc->encoder);
	enc->amf_frame_rate = AMFConstructRate(ovi.fps_num, ovi.fps_den);
	enc->fps_num = (int)ovi.fps_num;
	enc->fps_den = (int)ovi.fps_den;
	enc->full_range = ovi.range == VIDEO_RANGE_FULL;

	switch (ovi.colorspace) {
	case VIDEO_CS_601:
		enc->amf_color_profile =
			enc->full_range
				? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_601
				: AMF_VIDEO_CONVERTER_COLOR_PROFILE_601;
		enc->amf_primaries = AMF_COLOR_PRIMARIES_SMPTE170M;
		enc->amf_characteristic =
			AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE170M;
		break;
	case VIDEO_CS_DEFAULT:
		[[fallthrough]];
	case VIDEO_CS_709:
		enc->amf_color_profile =
			enc->full_range
				? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709
				: AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
		enc->amf_primaries = AMF_COLOR_PRIMARIES_BT709;
		enc->amf_characteristic =
			AMF_COLOR_TRANSFER_CHARACTERISTIC_BT709;
		break;
	case VIDEO_CS_SRGB:
		enc->amf_color_profile =
			enc->full_range
				? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_709
				: AMF_VIDEO_CONVERTER_COLOR_PROFILE_709;
		enc->amf_primaries = AMF_COLOR_PRIMARIES_BT709;
		enc->amf_characteristic =
			AMF_COLOR_TRANSFER_CHARACTERISTIC_IEC61966_2_1;
		break;
	case VIDEO_CS_2100_HLG:
		enc->amf_color_profile =
			enc->full_range
				? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020
				: AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
		enc->amf_primaries = AMF_COLOR_PRIMARIES_BT2020;
		enc->amf_characteristic =
			AMF_COLOR_TRANSFER_CHARACTERISTIC_ARIB_STD_B67;
		break;
	case VIDEO_CS_2100_PQ:
		enc->amf_color_profile =
			enc->full_range
				? AMF_VIDEO_CONVERTER_COLOR_PROFILE_FULL_2020
				: AMF_VIDEO_CONVERTER_COLOR_PROFILE_2020;
		enc->amf_primaries = AMF_COLOR_PRIMARIES_BT2020;
		enc->amf_characteristic =
			AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084;
		break;
	}

	switch (ovi.output_format) {
	case VIDEO_FORMAT_NV12:
		enc->amf_format = AMF_SURFACE_NV12;
		break;
	case VIDEO_FORMAT_P010:
		enc->amf_format = AMF_SURFACE_P010;
		break;
	}

	/* ------------------------------------ */
	/* create encoder                       */

	res = amf_factory->CreateContext(&enc->amf_context);
	if (res != AMF_OK)
		throw amf_error("CreateContext failed", res);

	res = enc->amf_context->InitDX11(enc->device, AMF_DX11_1);
	if (res != AMF_OK)
		throw amf_error("InitDX11 failed", res);

	res = amf_factory->CreateComponent(enc->amf_context,
					   enc->codec == amf_codec_type::HEVC
						   ? AMFVideoEncoder_HEVC
						   : AMFVideoEncoderVCE_AVC,
					   &enc->amf_encoder);
	if (res != AMF_OK)
		throw amf_error("CreateComponent failed", res);

	return true;

} catch (const amf_error &err) {
	error("%s: %s: %s", __FUNCTION__, err.str,
	      amf_trace->GetResultText(err.res));
	return false;
}

static void amf_destroy(void *data)
{
	amf_data *enc = (amf_data *)data;
	delete enc;
}

static void h264_video_info(void *, struct video_scale_info *info)
{
	info->format = VIDEO_FORMAT_NV12;
}

static void h265_video_info(void *, struct video_scale_info *info)
{
	switch (info->format) {
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_P010:
		info->format = VIDEO_FORMAT_P010;
		break;
	default:
		info->format = VIDEO_FORMAT_NV12;
	}
}

static void check_texture_encode_capability(obs_encoder_t *encoder, bool hevc)
{
	obs_video_info ovi;
	obs_get_video_info(&ovi);

	if (obs_encoder_scaling_enabled(encoder))
		throw "Encoder scaling is active";
	if (hevc) {
		if (!obs_nv12_tex_active() && !obs_p010_tex_active())
			throw "NV12/P010 textures aren't active";
	} else if (!obs_nv12_tex_active()) {
		throw "NV12 textures aren't active";
	}

	if ((hevc && !caps[ovi.adapter].supports_hevc) ||
	    (!hevc && !caps[ovi.adapter].supports_avc))
		throw "Wrong adapter";
}

#include "texture-amf-opts.hpp"

extern "C" void amf_defaults(obs_data_t *settings);
extern "C" obs_properties_t *amf_avc_properties(void *unused);
extern "C" obs_properties_t *amf_hevc_properties(void *unused);

/* ========================================================================= */
/* AVC Implementation                                                        */

static const char *amf_avc_get_name(void *)
{
	return "AMD HW H.264";
}

static inline int get_avc_preset(obs_data_t *settings)
{
	const char *preset = obs_data_get_string(settings, "preset");

	if (astrcmpi(preset, "balanced") == 0)
		return AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED;
	else if (astrcmpi(preset, "speed") == 0)
		return AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED;

	return AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY;
}

static inline int get_avc_rate_control(obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");

	if (astrcmpi(rc, "cqp") == 0)
		return AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP;
	else if (astrcmpi(rc, "vbr") == 0)
		return AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;

	return AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR;
}

static inline int get_avc_profile(obs_data_t *settings)
{
	const char *profile = obs_data_get_string(settings, "profile");

	if (astrcmpi(profile, "baseline") == 0)
		return AMF_VIDEO_ENCODER_PROFILE_BASELINE;
	else if (astrcmpi(profile, "main") == 0)
		return AMF_VIDEO_ENCODER_PROFILE_MAIN;
	else if (astrcmpi(profile, "constrained_baseline") == 0)
		return AMF_VIDEO_ENCODER_PROFILE_CONSTRAINED_BASELINE;
	else if (astrcmpi(profile, "constrained_high") == 0)
		return AMF_VIDEO_ENCODER_PROFILE_CONSTRAINED_HIGH;

	return AMF_VIDEO_ENCODER_PROFILE_HIGH;
}

static bool amf_avc_update(void *data, obs_data_t *settings)
{
	amf_data *enc = (amf_data *)data;

	int64_t bitrate = obs_data_get_int(settings, "bitrate") * 1000;
	int64_t qp = obs_data_get_int(settings, "cqp");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	const char *rc_str = obs_data_get_string(settings, "rate_control");

	int rc = get_avc_rate_control(settings);

	set_avc_property(enc, RATE_CONTROL_METHOD, rc);
	set_avc_property(enc, ENABLE_VBAQ, true);

	if (rc != AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP) {
		set_avc_property(enc, TARGET_BITRATE, bitrate);
		set_avc_property(enc, PEAK_BITRATE, bitrate * 15 / 10);
		set_avc_property(enc, VBV_BUFFER_SIZE, bitrate);
	} else {
		set_avc_property(enc, QP_I, qp);
		set_avc_property(enc, QP_P, qp);
		set_avc_property(enc, QP_B, qp);
	}

	set_avc_property(enc, ENFORCE_HRD, true);
	set_avc_property(enc, HIGH_MOTION_QUALITY_BOOST_ENABLE, false);

	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	int gop_size = (keyint_sec) ? keyint_sec * enc->fps_num / enc->fps_den
				    : 250;

	set_avc_property(enc, IDR_PERIOD, gop_size);
	set_avc_property(enc, DE_BLOCKING_FILTER, true);

	const char *ffmpeg_opts = obs_data_get_string(settings, "ffmpeg_opts");
	if (ffmpeg_opts && *ffmpeg_opts) {
		struct obs_options opts = obs_parse_options(ffmpeg_opts);
		for (size_t i = 0; i < opts.count; i++) {
			amf_apply_opt(enc, &opts.options[i]);
		}
		obs_free_options(opts);
	}

	if (!ffmpeg_opts || !*ffmpeg_opts)
		ffmpeg_opts = "(none)";

	info("settings:\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tpreset:       %s\n"
	     "\tprofile:      %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n"
	     "\tparams:       %s",
	     rc_str, bitrate, qp, gop_size, preset, profile, enc->cx, enc->cy,
	     ffmpeg_opts);

	return true;
}

static void *amf_avc_create(obs_data_t *settings, obs_encoder_t *encoder)
try {
	AMF_RESULT res;
	AMFVariant p;

	check_texture_encode_capability(encoder, false);

	amf_data *enc = new amf_data;
	enc->encoder = encoder;
	enc->codec = amf_codec_type::AVC;

	if (!amf_init_d3d11(enc))
		throw "Failed to create D3D11";
	if (!amf_create_encoder(enc))
		throw "Failed to create encoder";

	set_avc_property(enc, FRAMESIZE, AMFConstructSize(enc->cx, enc->cy));
	set_avc_property(enc, USAGE, AMF_VIDEO_ENCODER_USAGE_TRANSCONDING);
	set_avc_property(enc, QUALITY_PRESET, get_avc_preset(settings));
	set_avc_property(enc, PROFILE, get_avc_profile(settings));
	set_avc_property(enc, LOWLATENCY_MODE, false);
	set_avc_property(enc, CABAC_ENABLE, AMF_VIDEO_ENCODER_UNDEFINED);
	set_avc_property(enc, PRE_ANALYSIS_ENABLE,
			 AMF_VIDEO_ENCODER_PREENCODE_ENABLED);
	set_avc_property(enc, OUTPUT_COLOR_PROFILE, enc->amf_color_profile);
	set_avc_property(enc, OUTPUT_TRANSFER_CHARACTERISTIC,
			 enc->amf_characteristic);
	set_avc_property(enc, OUTPUT_COLOR_PRIMARIES, enc->amf_primaries);
	set_avc_property(enc, FULL_RANGE_COLOR, enc->full_range);

	amf_avc_update(enc, settings);

	res = enc->amf_encoder->Init(enc->amf_format, enc->cx, enc->cy);
	if (res != AMF_OK)
		throw amf_error("AMFComponent::Init failed", res);

	set_avc_property(enc, FRAMERATE, enc->amf_frame_rate);

	res = enc->amf_encoder->GetProperty(AMF_VIDEO_ENCODER_EXTRADATA, &p);
	if (res == AMF_OK && p.type == AMF_VARIANT_INTERFACE)
		enc->header = AMFBufferPtr(p.pInterface);

	return enc;

} catch (const amf_error &err) {
	blog(LOG_ERROR, "[texture-amf] %s: %s: %s", __FUNCTION__, err.str,
	     amf_trace->GetResultText(err.res));
	return obs_encoder_create_rerouted(encoder, "h264_ffmpeg_amf");

} catch (const char *err) {
	blog(LOG_ERROR, "[texture-amf] %s: %s", __FUNCTION__, err);
	return obs_encoder_create_rerouted(encoder, "h264_ffmpeg_amf");
}

static void register_avc()
{
	struct obs_encoder_info amf_encoder_info = {};
	amf_encoder_info.id = "h264_texture_amf";
	amf_encoder_info.type = OBS_ENCODER_VIDEO;
	amf_encoder_info.codec = "h264";
	amf_encoder_info.get_name = amf_avc_get_name;
	amf_encoder_info.create = amf_avc_create;
	amf_encoder_info.destroy = amf_destroy;
	amf_encoder_info.encode_texture = amf_encode_tex;
	amf_encoder_info.update = amf_avc_update;
	amf_encoder_info.get_defaults = amf_defaults;
	amf_encoder_info.get_properties = amf_avc_properties;
	amf_encoder_info.get_extra_data = amf_extra_data;
	amf_encoder_info.get_video_info = h264_video_info;
	amf_encoder_info.caps = OBS_ENCODER_CAP_PASS_TEXTURE;

	obs_register_encoder(&amf_encoder_info);
}

/* ========================================================================= */
/* HEVC Implementation                                                       */

#if ENABLE_HEVC

static const char *amf_hevc_get_name(void *)
{
	return "AMD HW H.265 (HEVC)";
}

static inline int get_hevc_preset(obs_data_t *settings)
{
	const char *preset = obs_data_get_string(settings, "preset");

	if (astrcmpi(preset, "balanced") == 0)
		return AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED;
	else if (astrcmpi(preset, "speed") == 0)
		return AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED;

	return AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY;
}

static inline int get_hevc_rate_control(obs_data_t *settings)
{
	const char *rc = obs_data_get_string(settings, "rate_control");

	if (astrcmpi(rc, "cqp") == 0)
		return AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CONSTANT_QP;
	else if (astrcmpi(rc, "vbr") == 0)
		return AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR;

	return AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR;
}

static bool amf_hevc_update(void *data, obs_data_t *settings)
{
	amf_data *enc = (amf_data *)data;

	int64_t bitrate = obs_data_get_int(settings, "bitrate") * 1000;
	int64_t qp = obs_data_get_int(settings, "cqp");
	const char *preset = obs_data_get_string(settings, "preset");
	const char *profile = obs_data_get_string(settings, "profile");
	const char *rc_str = obs_data_get_string(settings, "rate_control");

	int rc = get_hevc_rate_control(settings);

	set_hevc_property(enc, RATE_CONTROL_METHOD, rc);
	set_hevc_property(enc, ENABLE_VBAQ, true);

	if (rc != AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP) {
		int64_t bitrate = obs_data_get_int(settings, "bitrate") * 1000;
		set_hevc_property(enc, TARGET_BITRATE, bitrate);
		set_hevc_property(enc, PEAK_BITRATE, bitrate * 15 / 10);
		set_hevc_property(enc, VBV_BUFFER_SIZE, bitrate);
	} else {
		int64_t qp = obs_data_get_int(settings, "cqp");
		set_hevc_property(enc, QP_I, qp);
		set_hevc_property(enc, QP_P, qp);
	}

	set_hevc_property(enc, ENFORCE_HRD, true);
	set_hevc_property(enc, HIGH_MOTION_QUALITY_BOOST_ENABLE, false);

	int keyint_sec = (int)obs_data_get_int(settings, "keyint_sec");
	int gop_size = (keyint_sec) ? keyint_sec * enc->fps_num / enc->fps_den
				    : 250;

	set_hevc_property(enc, NUM_GOPS_PER_IDR, gop_size);

	const char *ffmpeg_opts = obs_data_get_string(settings, "ffmpeg_opts");
	if (ffmpeg_opts && *ffmpeg_opts) {
		struct obs_options opts = obs_parse_options(ffmpeg_opts);
		for (size_t i = 0; i < opts.count; i++) {
			amf_apply_opt(enc, &opts.options[i]);
		}
		obs_free_options(opts);
	}

	if (!ffmpeg_opts || !*ffmpeg_opts)
		ffmpeg_opts = "(none)";

	info("settings:\n"
	     "\trate_control: %s\n"
	     "\tbitrate:      %d\n"
	     "\tcqp:          %d\n"
	     "\tkeyint:       %d\n"
	     "\tpreset:       %s\n"
	     "\tprofile:      %s\n"
	     "\twidth:        %d\n"
	     "\theight:       %d\n"
	     "\tparams:       %s",
	     rc_str, bitrate, qp, gop_size, preset, profile, enc->cx, enc->cy,
	     ffmpeg_opts);

	return true;
}

static inline bool is_hlg(amf_data *enc)
{
	return enc->amf_characteristic ==
	       AMF_COLOR_TRANSFER_CHARACTERISTIC_ARIB_STD_B67;
}

static inline bool is_pq(amf_data *enc)
{
	return enc->amf_characteristic ==
	       AMF_COLOR_TRANSFER_CHARACTERISTIC_SMPTE2084;
}

constexpr amf_uint16 amf_hdr_primary(uint32_t num, uint32_t den)
{
	return (amf_uint16)(num * 50000 / den);
}

constexpr amf_uint32 lum_mul = 10000;
constexpr float lum_mul_f = (float)lum_mul;

constexpr amf_uint32 amf_make_lum(amf_uint32 val)
{
	return val * lum_mul;
}

static inline amf_uint32 amf_nominal_level()
{
	return (amf_uint32)(obs_get_video_hdr_nominal_peak_level() * lum_mul_f);
}

static void *amf_hevc_create(obs_data_t *settings, obs_encoder_t *encoder)
try {
	AMF_RESULT res;
	AMFVariant p;

	check_texture_encode_capability(encoder, true);

	amf_data *enc = new amf_data;
	enc->encoder = encoder;
	enc->codec = amf_codec_type::HEVC;

	if (!amf_init_d3d11(enc))
		throw "Failed to create D3D11";
	if (!amf_create_encoder(enc))
		throw "Failed to create encoder";

	const bool is10bit = enc->amf_format == AMF_SURFACE_P010;
	const bool pq = is_pq(enc);
	const bool hlg = is_hlg(enc);
	const bool is_hdr = pq || hlg;

	set_hevc_property(enc, FRAMESIZE, AMFConstructSize(enc->cx, enc->cy));
	set_hevc_property(enc, USAGE, AMF_VIDEO_ENCODER_USAGE_TRANSCONDING);
	set_hevc_property(enc, QUALITY_PRESET, get_hevc_preset(settings));
	set_hevc_property(enc, COLOR_BIT_DEPTH,
			  is10bit ? AMF_COLOR_BIT_DEPTH_10
				  : AMF_COLOR_BIT_DEPTH_8);
	set_hevc_property(enc, PROFILE,
			  is10bit ? AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10
				  : AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN);
	set_hevc_property(enc, LOWLATENCY_MODE, false);
	set_hevc_property(enc, OUTPUT_COLOR_PROFILE, enc->amf_color_profile);
	set_hevc_property(enc, OUTPUT_TRANSFER_CHARACTERISTIC,
			  enc->amf_characteristic);
	set_hevc_property(enc, OUTPUT_COLOR_PRIMARIES, enc->amf_primaries);
	set_hevc_property(enc, NOMINAL_RANGE, enc->full_range);

	if (is_hdr) {
		AMFBufferPtr buf;
		enc->amf_context->AllocBuffer(AMF_MEMORY_HOST,
					      sizeof(AMFHDRMetadata), &buf);
		AMFHDRMetadata *md = (AMFHDRMetadata *)buf->GetNative();
		md->redPrimary[0] = amf_hdr_primary(17, 25);
		md->redPrimary[1] = amf_hdr_primary(8, 25);
		md->greenPrimary[0] = amf_hdr_primary(53, 200);
		md->greenPrimary[1] = amf_hdr_primary(69, 100);
		md->bluePrimary[0] = amf_hdr_primary(3, 20);
		md->bluePrimary[1] = amf_hdr_primary(3, 50);
		md->whitePoint[0] = amf_hdr_primary(3127, 10000);
		md->whitePoint[1] = amf_hdr_primary(329, 1000);
		md->minMasteringLuminance = 0;
		md->maxMasteringLuminance = pq ? amf_nominal_level()
					       : (hlg ? amf_make_lum(1000) : 0);
		md->maxContentLightLevel = 0;
		md->maxFrameAverageLightLevel = 0;
		set_hevc_property(enc, INPUT_HDR_METADATA, buf);
	}

	amf_hevc_update(enc, settings);

	res = enc->amf_encoder->Init(enc->amf_format, enc->cx, enc->cy);
	if (res != AMF_OK)
		throw amf_error("AMFComponent::Init failed", res);

	set_hevc_property(enc, FRAMERATE, enc->amf_frame_rate);

	res = enc->amf_encoder->GetProperty(AMF_VIDEO_ENCODER_HEVC_EXTRADATA,
					    &p);
	if (res == AMF_OK && p.type == AMF_VARIANT_INTERFACE)
		enc->header = AMFBufferPtr(p.pInterface);

	return enc;

} catch (const amf_error &err) {
	blog(LOG_ERROR, "[texture-amf] %s: %s: %S", __FUNCTION__, err.str,
	     amf_trace->GetResultText(err.res));
	return obs_encoder_create_rerouted(encoder, "h265_ffmpeg_amf");

} catch (const char *err) {
	blog(LOG_ERROR, "[texture-amf] %s: %s", __FUNCTION__, err);
	return obs_encoder_create_rerouted(encoder, "h265_ffmpeg_amf");
}

static void register_hevc()
{
	struct obs_encoder_info amf_encoder_info = {};
	amf_encoder_info.id = "h265_texture_amf";
	amf_encoder_info.type = OBS_ENCODER_VIDEO;
	amf_encoder_info.codec = "hevc";
	amf_encoder_info.get_name = amf_hevc_get_name;
	amf_encoder_info.create = amf_hevc_create;
	amf_encoder_info.destroy = amf_destroy;
	amf_encoder_info.encode_texture = amf_encode_tex;
	amf_encoder_info.update = amf_hevc_update;
	amf_encoder_info.get_defaults = amf_defaults;
	amf_encoder_info.get_properties = amf_hevc_properties;
	amf_encoder_info.get_extra_data = amf_extra_data;
	amf_encoder_info.get_video_info = h265_video_info;
	amf_encoder_info.caps = OBS_ENCODER_CAP_PASS_TEXTURE;

	obs_register_encoder(&amf_encoder_info);
}

#endif //ENABLE_HEVC

/* ========================================================================= */
/* Global Stuff                                                              */

extern "C" void amf_load(void)
try {
	AMF_RESULT res;

	amf_module = LoadLibraryW(AMF_DLL_NAME);
	if (!amf_module)
		throw "No AMF library";

	/* ----------------------------------- */
	/* Check for AVC/HEVC support          */

	BPtr<char> test_exe = os_get_executable_path_ptr("obs-amf-test.exe");
	std::string caps_str;

	os_process_pipe_t *pp = os_process_pipe_create(test_exe, "r");
	if (!pp)
		throw "Failed to launch the AMF test process I guess";

	for (;;) {
		char data[2048];
		size_t len =
			os_process_pipe_read(pp, (uint8_t *)data, sizeof(data));
		if (!len)
			break;

		caps_str.append(data, len);
	}

	os_process_pipe_destroy(pp);

	if (caps_str.empty())
		throw "Seems the AMF test subprocess crashed. "
		      "Better there than here I guess. "
		      "Let's just skip loading AMF then I suppose.";

	ConfigFile config;
	if (config.OpenString(caps_str.c_str()) != 0)
		throw "Failed to open config string";

	const char *error = config_get_string(config, "error", "string");
	if (error)
		throw std::string(error);

	uint32_t adapter_count = (uint32_t)config_num_sections(config);
	bool avc_supported = false;
	bool hevc_supported = false;

	for (uint32_t i = 0; i < adapter_count; i++) {
		std::string section = std::to_string(i);
		adapter_caps &info = caps[i];

		info.is_amd =
			config_get_bool(config, section.c_str(), "is_amd");
		info.supports_avc = config_get_bool(config, section.c_str(),
						    "supports_avc");
		info.supports_hevc = config_get_bool(config, section.c_str(),
						     "supports_hevc");

		avc_supported |= info.supports_avc;
		hevc_supported |= info.supports_hevc;
	}

	if (!avc_supported || !hevc_supported)
		throw "Neither AVC nor HEVC are supported by any devices";

	/* ----------------------------------- */
	/* Init AMF                            */

	AMFInit_Fn init =
		(AMFInit_Fn)GetProcAddress(amf_module, AMF_INIT_FUNCTION_NAME);
	if (!init)
		throw "Failed to get AMFInit address";

	res = init(AMF_FULL_VERSION, &amf_factory);
	if (res != AMF_OK)
		throw amf_error("AMFInit failed", res);

	res = amf_factory->GetTrace(&amf_trace);
	if (res != AMF_OK)
		throw amf_error("GetTrace failed", res);

	AMFQueryVersion_Fn get_ver = (AMFQueryVersion_Fn)GetProcAddress(
		amf_module, AMF_QUERY_VERSION_FUNCTION_NAME);
	if (!get_ver)
		throw "Failed to get AMFQueryVersion address";

	res = get_ver(&amf_version);
	if (res != AMF_OK)
		throw amf_error("AMFQueryVersion failed", res);

#ifndef DEBUG_AMF_STUFF
	amf_trace->EnableWriter(AMF_TRACE_WRITER_DEBUG_OUTPUT, false);
	amf_trace->EnableWriter(AMF_TRACE_WRITER_CONSOLE, false);
#endif

	/* ----------------------------------- */
	/* Register encoders                   */

	if (avc_supported)
		register_avc();
#if ENABLE_HEVC
	if (hevc_supported)
		register_hevc();
#endif

} catch (const std::string &str) {
	/* doing debug here because string exceptions indicate the user is
	 * probably not using AMD */
	blog(LOG_DEBUG, "%s: %s", __FUNCTION__, str.c_str());

} catch (const char *str) {
	/* doing debug here because string exceptions indicate the user is
	 * probably not using AMD */
	blog(LOG_DEBUG, "%s: %s", __FUNCTION__, str);

} catch (const amf_error &err) {
	/* doing an error here because it means at least the library has loaded
	 * successfully, so they probably have AMD at this point */
	blog(LOG_ERROR, "%s: %s: 0x%lX", __FUNCTION__, err.str,
	     (uint32_t)err.res);
}

extern "C" void amf_unload(void)
{
	if (amf_module && amf_trace) {
		amf_trace->TraceFlush();
		amf_trace->UnregisterWriter(L"obs_amf_trace_writer");
	}
}
