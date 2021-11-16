#include <cuda.h>
#include <cuviddec.h>
#include <nvcuvid.h>
#include <va/va_backend.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vabackend.h"
#include <drm/drm_fourcc.h>

#include <cudaEGL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <unistd.h>
#include <fcntl.h>

//codecs
#include "mpeg2.h"
#include "h264.h"
#include "vp9.h"
#include "vp8.h"
#include "mpeg4.h"

#include <sys/syscall.h>

#ifndef EGL_NV_stream_consumer_eglimage
#define EGL_NV_stream_consumer_eglimage 1
#define EGL_STREAM_CONSUMER_IMAGE_NV      0x3373
#define EGL_STREAM_IMAGE_ADD_NV           0x3374
#define EGL_STREAM_IMAGE_REMOVE_NV        0x3375
#define EGL_STREAM_IMAGE_AVAILABLE_NV     0x3376
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
typedef EGLint (EGLAPIENTRYP PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMACQUIREIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMRELEASEIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#ifdef EGL_EGLEXT_PROTOTYPES
EGLAPI EGLBoolean EGLAPIENTRY eglStreamImageConsumerConnectNV (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
EGLAPI EGLint EGLAPIENTRY eglQueryStreamConsumerEventNV (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamAcquireImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamReleaseImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#endif
#endif /* EGL_NV_stream_consumer_eglimage */

PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC eglQueryStreamConsumerEventNV;
PFNEGLSTREAMRELEASEIMAGENVPROC eglStreamReleaseImageNV;
PFNEGLSTREAMACQUIREIMAGENVPROC eglStreamAcquireImageNV;
PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA;
PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA;

uint64_t gettid() {
    return syscall(SYS_gettid);
}

#define LOG(msg, ...) printf("[%lx] " __FILE__ ":%4d %24s " msg, gettid(), __LINE__, __FUNCTION__ __VA_OPT__(,) __VA_ARGS__);

void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s\n", message);
}

void __checkCudaErrors(CUresult err, const char *file, const int line)
{
    if (CUDA_SUCCESS != err)
    {
        const char *errStr = NULL;
        cuGetErrorString(err, &errStr);
        LOG("cuda error '%s' (%d) at file <%s>, line %i.\n", errStr, err, file, line);
        exit(EXIT_FAILURE);
    }
}
#define checkCudaErrors(err)  __checkCudaErrors(err, __FILE__, __LINE__)

NVCodecHolder *codecs = NULL;

void registerCodec(NVCodec *codec) {
    NVCodecHolder *newCodecHolder = (NVCodecHolder*) calloc(1, sizeof(NVCodecHolder));
    newCodecHolder->codec = codec;
    newCodecHolder->next = codecs;
    codecs = newCodecHolder;
}

void appendBuffer(AppendableBuffer *ab, void *buf, uint64_t size)
{
  if (ab->buf == NULL) {
      ab->allocated = size*2;
      ab->buf = aligned_alloc(16, ab->allocated);
      ab->size = 0;
  } else if (ab->size + size > ab->allocated) {
      while (ab->size + size > ab->allocated) {
        ab->allocated += ab->allocated >> 1;
      }
      void *nb = aligned_alloc(16, ab->allocated);
      memcpy(nb, ab->buf, ab->size);
      free(ab->buf);
      ab->buf = nb;
  }
  memcpy(ab->buf + ab->size, buf, size);
  ab->size += size;
}

void freeBuffer(AppendableBuffer *ab) {
  if (ab->buf != NULL) {
      free(ab->buf);
      ab->buf = NULL;
      ab->size = 0;
      ab->allocated = 0;
  }
}

Object allocateObject(NVDriver *drv, ObjectType type, int allocatePtrSize)
{
    Object newObj = (Object) calloc(1, sizeof(struct Object_t));

    newObj->type = type;
    newObj->id = (++drv->nextObjId);

    if (allocatePtrSize > 0) {
        newObj->obj = calloc(1, allocatePtrSize);
    }

    if (drv->objRoot == NULL) {
        drv->objRoot = newObj;
    } else {
        Object o = drv->objRoot;
        while (o->next != NULL) {
            o = (Object) (o->next);
        }

        o->next = (struct Object_t *) newObj;
    }

    return newObj;
}

Object getObject(NVDriver *drv, VAGenericID id) {
    if (id != VA_INVALID_ID) {
        for (Object o = drv->objRoot; o != NULL; o = o->next) {
            if (o->id == id) {
                return o;
            }
        }
    }
    return NULL;
}

Object getObjectByPtr(NVDriver *drv, void *ptr) {
    if (ptr != NULL) {
        for (Object o = drv->objRoot; o != NULL; o = o->next) {
            if (o->obj == ptr) {
                return o;
            }
        }
    }
    return NULL;
}

void deleteObject(NVDriver *drv, VAGenericID id) {
    if (drv->objRoot == NULL || id == VA_INVALID_ID) {
        return;
    } else if (drv->objRoot->id == id) {
        Object o = drv->objRoot;
        drv->objRoot = (Object) drv->objRoot->next;
        free(o);
    } else {
        Object last = NULL;
        Object o = drv->objRoot;
        while (o != NULL && o->id != id) {
            last = o;
            o = (Object) o->next;
        }

        if (o != NULL) {
            last->next = o->next;
            free(o);
        }
    }
}

int pictureIdxFromSurfaceId(NVDriver *drv, VASurfaceID surf) {
    Object obj = getObject(drv, surf);
    if (obj != NULL && obj->type == OBJECT_TYPE_SURFACE) {
        NVSurface *suf = (NVSurface*) obj->obj;
        return suf->pictureIdx;
    }
    return -1;
}

cudaVideoCodec vaToCuCodec(VAProfile profile)
{
    for (NVCodecHolder *c = codecs; c != NULL; c = c->next) {
        cudaVideoCodec cvc = c->codec->computeCudaCodec(profile);
        if (cvc != cudaVideoCodec_NONE) {
            return cvc;
        }
    }

    LOG("vaToCuCodec: Unknown codec: %d\n", profile);
    return cudaVideoCodec_NONE;
}

VAStatus nvTerminate( VADriverContextP ctx )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

int doesGPUSupportCodec(cudaVideoCodec codec, int bitDepth, cudaVideoChromaFormat chromaFormat, int *width, int *height)
{
    CUVIDDECODECAPS videoDecodeCaps;
    memset(&videoDecodeCaps, 0, sizeof(CUVIDDECODECAPS));
    videoDecodeCaps.eCodecType      = codec;
    videoDecodeCaps.eChromaFormat   = chromaFormat;
    videoDecodeCaps.nBitDepthMinus8 = bitDepth - 8;

    CUresult result = cuvidGetDecoderCaps(&videoDecodeCaps);
    if (result != CUDA_SUCCESS)
    {
        return 0;
    }
    if (width != NULL) {
        *width = videoDecodeCaps.nMaxWidth;
    }
    if (height != NULL) {
        *height = videoDecodeCaps.nMaxHeight;
    }
    return videoDecodeCaps.bIsSupported;
}

#define MAX_PROFILES 32
VAStatus nvQueryConfigProfiles(
        VADriverContextP ctx,
        VAProfile *profile_list,	/* out */
        int *num_profiles			/* out */
    )
{
    LOG("In %s\n", __FUNCTION__);

    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    cuCtxSetCurrent(drv->g_oContext);

    int profiles = 0;
    if (doesGPUSupportCodec(cudaVideoCodec_MPEG2, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG2Simple;
        profile_list[profiles++] = VAProfileMPEG2Main;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_MPEG4, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileMPEG4Simple;
        profile_list[profiles++] = VAProfileMPEG4AdvancedSimple;
        profile_list[profiles++] = VAProfileMPEG4Main;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VC1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVC1Simple;
        profile_list[profiles++] = VAProfileVC1Main;
        profile_list[profiles++] = VAProfileVC1Advanced;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264Baseline;
        profile_list[profiles++] = VAProfileH264Main;
        profile_list[profiles++] = VAProfileH264High;
        profile_list[profiles++] = VAProfileH264ConstrainedBaseline;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_JPEG, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileJPEGBaseline;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264_SVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264StereoHigh;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_H264_MVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileH264MultiviewHigh;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain10;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_10;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain422_12;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain444;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain444_10;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain444_12;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_HEVC, 12, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileHEVCMain12;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP8, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP8Version0_3;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile0; //color depth: 8 bit, 4:2:0
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP9, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile1; //color depth: 8 bit, 4:2:2, 4:4:0, 4:4:4
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile2; //color depth: 10–12 bit, 4:2:0
    }
    if (doesGPUSupportCodec(cudaVideoCodec_VP9, 10, cudaVideoChromaFormat_444, NULL, NULL)) {
        profile_list[profiles++] = VAProfileVP9Profile3; //color depth: 10–12 bit, :2:2, 4:4:0, 4:4:4
    }
    if (doesGPUSupportCodec(cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_420, NULL, NULL)) {
        profile_list[profiles++] = VAProfileAV1Profile0;
    }
    if (doesGPUSupportCodec(cudaVideoCodec_AV1, 8, cudaVideoChromaFormat_444, NULL, NULL)) {
        profile_list[profiles++] = VAProfileAV1Profile1;
    }
    *num_profiles = profiles;

    return VA_STATUS_SUCCESS;
}

VAStatus nvQueryConfigEntrypoints(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint  *entrypoint_list,	/* out */
        int *num_entrypoints			/* out */
    )
{
    //LOG("In %s\n", __FUNCTION__);

    entrypoint_list[0] = VAEntrypointVLD;
    *num_entrypoints = 1;

    return VA_STATUS_SUCCESS;
}

VAStatus nvGetConfigAttributes(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,	/* in/out */
        int num_attribs
    )
{
    LOG("In %s\n", __FUNCTION__);

    if (vaToCuCodec(profile) == cudaVideoCodec_NONE) {
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    for (int i = 0; i < num_attribs; i++)
    {
        if (attrib_list[i].type == VAConfigAttribRTFormat)
        {
            attrib_list[i].value = VA_RT_FORMAT_YUV420;

            if (profile == VAProfileHEVCMain10) {
                attrib_list[i].value |= VA_RT_FORMAT_YUV420_10;
            }
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureWidth)
        {
            doesGPUSupportCodec(vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, &attrib_list[i].value, NULL);
        }
        else if (attrib_list[i].type == VAConfigAttribMaxPictureHeight)
        {
            doesGPUSupportCodec(vaToCuCodec(profile), 8, cudaVideoChromaFormat_420, NULL, &attrib_list[i].value);
        }
        else
        {
            LOG("unhandled config attribute: %d\n", attrib_list[i].type);
        }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nvCreateConfig(
        VADriverContextP ctx,
        VAProfile profile,
        VAEntrypoint entrypoint,
        VAConfigAttrib *attrib_list,
        int num_attribs,
        VAConfigID *config_id		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s with profile: %d with %d attributes\n", __FUNCTION__, profile, num_attribs);

    Object obj = allocateObject(drv, OBJECT_TYPE_CONFIG, sizeof(NVConfig));
    NVConfig *cfg = (NVConfig*) obj->obj;
    cfg->profile = profile;
    cfg->entrypoint = entrypoint;
    cfg->attrib_list = attrib_list; //TODO might need to make a copy of this
    cfg->num_attribs = num_attribs;

    for (int i = 0; i < num_attribs; i++)
    {
      LOG("got config attrib: %d %d %d\n", i, attrib_list[i].type, attrib_list[i].value);
    }

    cfg->cudaCodec = vaToCuCodec(cfg->profile);

    if (profile == VAProfileHEVCMain10) {
        cfg->surfaceFormat = cudaVideoSurfaceFormat_P016;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->bitDepth = 10;
    } else {
        cfg->surfaceFormat = cudaVideoSurfaceFormat_NV12;
        cfg->chromaFormat = cudaVideoChromaFormat_420;
        cfg->bitDepth = 8;
    }

    *config_id = obj->id;

    return VA_STATUS_SUCCESS;
}

VAStatus nvDestroyConfig(
        VADriverContextP ctx,
        VAConfigID config_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s\n", __FUNCTION__);


    deleteObject(drv, config_id);

    return VA_STATUS_SUCCESS;
}

VAStatus nvQueryConfigAttributes(
        VADriverContextP ctx,
        VAConfigID config_id,
        VAProfile *profile,		/* out */
        VAEntrypoint *entrypoint, 	/* out */
        VAConfigAttrib *attrib_list,	/* out */
        int *num_attribs		/* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s\n", __FUNCTION__);

    Object obj = getObject(drv, config_id);

    if (obj != NULL)
    {
        NVConfig *cfg = (NVConfig*) obj->obj;
        *profile = cfg->profile;
        *entrypoint = cfg->entrypoint;
        //*attrib_list = cfg->attrib_list; //TODO is that the right thing/type?
        *num_attribs = cfg->num_attribs;
        return VA_STATUS_SUCCESS;
    }

    return VA_STATUS_ERROR_INVALID_CONFIG;
}

VAStatus nvCreateSurfaces2(
            VADriverContextP    ctx,
            unsigned int        format,
            unsigned int        width,
            unsigned int        height,
            VASurfaceID        *surfaces,
            unsigned int        num_surfaces,
            VASurfaceAttrib    *attrib_list,
            unsigned int        num_attribs
        )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s\n", __FUNCTION__);
    LOG("creating %d surface(s) %dx%d, format %X\n", num_surfaces, width, height, format);

    cudaVideoSurfaceFormat nvFormat;
    int bitdepth;

    if (format == VA_RT_FORMAT_YUV420) {
        nvFormat = cudaVideoSurfaceFormat_NV12;
        bitdepth = 8;
    } else if (format == VA_RT_FORMAT_YUV420_10) {
        nvFormat = cudaVideoSurfaceFormat_P016;
        bitdepth = 10;
    } else if (format == VA_RT_FORMAT_YUV420_12) {
        nvFormat = cudaVideoSurfaceFormat_P016;
        bitdepth = 12;
    } else {
        LOG("Unknown format: %X\n", format);
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }

    for (int i = 0; i < num_surfaces; i++)
    {
        Object surfaceObject = allocateObject(drv, OBJECT_TYPE_SURFACE, sizeof(NVSurface));
        LOG("created surface %d\n", surfaceObject->id);
        surfaces[i] = surfaceObject->id;
        NVSurface *suf = (NVSurface*) surfaceObject->obj;
        suf->width = width;
        suf->height = height;
        suf->format = nvFormat;
        suf->pictureIdx = -1;
        suf->bitdepth = bitdepth;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nvCreateSurfaces(
        VADriverContextP ctx,
        int width,
        int height,
        int format,
        int num_surfaces,
        VASurfaceID *surfaces		/* out */
    )
{
    LOG("In %s\n", __FUNCTION__);
    return nvCreateSurfaces2(ctx, format, width, height, surfaces, num_surfaces, NULL, 0);
}


VAStatus nvDestroySurfaces(
        VADriverContextP ctx,
        VASurfaceID *surface_list,
        int num_surfaces
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s\n", __FUNCTION__);

    for (int i = 0; i < num_surfaces; i++)
    {
        deleteObject(drv, surface_list[i]);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nvCreateContext(
        VADriverContextP ctx,
        VAConfigID config_id,
        int picture_width,
        int picture_height,
        int flag,
        VASurfaceID *render_targets,
        int num_render_targets,
        VAContextID *context		/* out */
    )
{
    LOG("In %s with %d render targets\n", __FUNCTION__, num_render_targets);

    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVConfig *cfg = (NVConfig*) getObject(drv, config_id)->obj;

    CUvideodecoder decoder;
    CUVIDDECODECREATEINFO vdci;
    memset(&vdci, 0, sizeof(CUVIDDECODECREATEINFO));
    vdci.ulWidth = picture_width;
    vdci.ulHeight = picture_height;
    vdci.ulNumDecodeSurfaces = num_render_targets; //TODO is this correct? probably not, but the about of decode surfaces needed is determined by codec, i think
    vdci.CodecType = cfg->cudaCodec;
    vdci.ulCreationFlags = cudaVideoCreate_PreferCUVID;
    vdci.ulIntraDecodeOnly = 0; //TODO (flag & VA_PROGRESSIVE) != 0
    vdci.display_area.right = picture_width;
    vdci.display_area.bottom = picture_height;

    vdci.ChromaFormat = cfg->chromaFormat;
    vdci.OutputFormat = cfg->surfaceFormat;
    vdci.bitDepthMinus8 = cfg->bitDepth - 8;

    vdci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
    vdci.ulTargetWidth = picture_width;
    vdci.ulTargetHeight = picture_height;
    vdci.ulNumOutputSurfaces = num_render_targets;

    cuvidCtxLockCreate(&vdci.vidLock, drv->g_oContext);

    CUresult result = cuvidCreateDecoder(&decoder, &vdci);

    if (result != CUDA_SUCCESS)
    {
        LOG("cuvidCreateDecoder failed: %d\n", result);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    Object contextObj = allocateObject(drv, OBJECT_TYPE_CONTEXT, sizeof(NVContext));
    *context = contextObj->id;

    NVContext *nvCtx = (NVContext*) contextObj->obj;
    nvCtx->drv = drv;
    nvCtx->decoder = decoder;
    nvCtx->profile = cfg->profile;
    nvCtx->entrypoint = cfg->entrypoint;
    nvCtx->width = picture_width;
    nvCtx->height = picture_height;

    for (NVCodecHolder *c = codecs; c != NULL; c = c->next) {
        for (int i = 0; i < c->codec->supportedProfileCount; i++) {
            if (c->codec->supportedProfiles[i] == cfg->profile) {
                nvCtx->codec = c->codec;
                break;
            }
        }
    }
    LOG("got codec: %p\n", nvCtx->codec);

    //assign all the render targets unique ids up-front
    //this seems to be a simplier way to manage them
    for (int i = 0; i < num_render_targets; i++) {
        Object obj = getObject(drv, render_targets[i]);
        NVSurface *suf = obj->obj;
        LOG("assigning surface id %d to picture index %d\n", obj->id, i);
        suf->pictureIdx = i;
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nvDestroyContext(
        VADriverContextP ctx,
        VAContextID context)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s\n", __FUNCTION__);

    NVContext *nvCtx = (NVContext*) getObject(drv, context)->obj;

    if (nvCtx != NULL)
    {
      CUvideodecoder decoder = nvCtx->decoder;
      nvCtx->decoder = NULL;
      freeBuffer(&nvCtx->slice_offsets);
      freeBuffer(&nvCtx->buf);

      //delete the NVContext object before we try to free the decoder
      //so if that fails we don't leak
      deleteObject(drv, context);

      if (decoder != NULL)
      {
        CUresult result = cuvidDestroyDecoder(decoder);
        if (result != CUDA_SUCCESS)
        {
            LOG("cuvidDestroyDecoder failed: %d\n", result);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
      }
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nvCreateBuffer(
        VADriverContextP ctx,
        VAContextID context,		/* in */
        VABufferType type,		/* in */
        unsigned int size,		/* in */
        unsigned int num_elements,	/* in */
        void *data,			/* in */
        VABufferID *buf_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    //LOG("In %s\n", __FUNCTION__);

    //TODO should pool these as most of the time these should be the same size
    Object bufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    *buf_id = bufferObject->id;

    NVBuffer *buf = (NVBuffer*) bufferObject->obj;
    buf->bufferType = type;
    buf->elements = num_elements;
    buf->size = num_elements * size;
    buf->ptr = aligned_alloc(16, buf->size);

    if (data != NULL)
    {
        memcpy(buf->ptr, data, buf->size);
    }

    return VA_STATUS_SUCCESS;
}

VAStatus nvBufferSetNumElements(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        unsigned int num_elements	/* in */
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvMapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id,	/* in */
        void **pbuf         /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s with %d\n", __FUNCTION__, buf_id);

    Object obj = getObject(drv, buf_id);
    NVBuffer *buf = (NVBuffer*) obj->obj;
    *pbuf = buf->ptr;

    return VA_STATUS_SUCCESS;
}

VAStatus nvUnmapBuffer(
        VADriverContextP ctx,
        VABufferID buf_id	/* in */
    )
{
    LOG("In %s\n", __FUNCTION__);

    return VA_STATUS_SUCCESS;
}

VAStatus nvDestroyBuffer(
        VADriverContextP ctx,
        VABufferID buffer_id
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    //LOG("In %s\n", __FUNCTION__);

    Object obj = getObject(drv, buffer_id);

    deleteObject(drv, buffer_id);

    return VA_STATUS_SUCCESS;
}

VAStatus nvBeginPicture(
        VADriverContextP ctx,
        VAContextID context,
        VASurfaceID render_target
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    //LOG("In %s\n", __FUNCTION__);

    NVContext *nvCtx = (NVContext*) getObject(drv, context)->obj;
    memset(&nvCtx->pPicParams, 0, sizeof(CUVIDPICPARAMS));
    nvCtx->render_target = (NVSurface*) getObject(drv, render_target)->obj;

    return VA_STATUS_SUCCESS;
}

VAStatus nvRenderPicture(
        VADriverContextP ctx,
        VAContextID context,
        VABufferID *buffers,
        int num_buffers
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    //LOG("In %s\n", __FUNCTION__);

    NVContext *nvCtx = (NVContext*) getObject(drv, context)->obj;
    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;
    VAProfile prof = nvCtx->profile;
    cudaVideoCodec codec = vaToCuCodec(prof);

    //LOG("got %d buffers\n", num_buffers);
    for (int i = 0; i < num_buffers; i++)
    {
        //LOG("got buffer %d\n", buffers[i]);
        NVBuffer *buf = (NVBuffer*) getObject(drv, buffers[i])->obj;
        VABufferType bt = buf->bufferType;
        HandlerFunc func = nvCtx->codec->handlers[bt];
        if (func != NULL) {
            func(nvCtx, buf, picParams);
        } else {
            LOG("Unhandled buffer type: %d\n", bt);
        }
    }
    return VA_STATUS_SUCCESS;
}

VAStatus nvEndPicture(
        VADriverContextP ctx,
        VAContextID context
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    NVContext *nvCtx = (NVContext*) getObject(drv, context)->obj;

    CUVIDPICPARAMS *picParams = &nvCtx->pPicParams;

    picParams->pBitstreamData = nvCtx->buf.buf;
    picParams->pSliceDataOffsets = nvCtx->slice_offsets.buf;
    nvCtx->buf.size = 0;
    nvCtx->slice_offsets.size = 0;

    picParams->CurrPicIdx = nvCtx->render_target->pictureIdx;

    CUresult result = cuvidDecodePicture(nvCtx->decoder, picParams);

    if (result != CUDA_SUCCESS)
    {
        LOG("cuvidDecodePicture failed: %d\n", result);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    LOG("cuvid decoded successful to idx: %d\n", picParams->CurrPicIdx);
    nvCtx->render_target->decoder = nvCtx->decoder;
    nvCtx->render_target->progressive_frame = !picParams->field_pic_flag;
    nvCtx->render_target->top_field_first = !picParams->bottom_field_flag;
    nvCtx->render_target->second_field = picParams->second_field;

    return VA_STATUS_SUCCESS;
}

VAStatus nvSyncSurface(
        VADriverContextP ctx,
        VASurfaceID render_target
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_SUCCESS;
}

VAStatus nvQuerySurfaceStatus(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VASurfaceStatus *status	/* out */
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvQuerySurfaceError(
        VADriverContextP ctx,
        VASurfaceID render_target,
        VAStatus error_status,
        void **error_info /*out*/
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvPutSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        void* draw, /* Drawable of window system */
        short srcx,
        short srcy,
        unsigned short srcw,
        unsigned short srch,
        short destx,
        short desty,
        unsigned short destw,
        unsigned short desth,
        VARectangle *cliprects, /* client supplied clip list */
        unsigned int number_cliprects, /* number of clip rects in the clip list */
        unsigned int flags /* de-interlacing flags */
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvQueryImageFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        int *num_formats           /* out */
    )
{
    LOG("In %s\n", __FUNCTION__);

    format_list[0].fourcc = VA_FOURCC_NV12;
    format_list[0].byte_order = VA_LSB_FIRST;
    format_list[0].bits_per_pixel = 12;

    format_list[1].fourcc = VA_FOURCC_P010;
    format_list[1].byte_order = VA_LSB_FIRST;
    format_list[1].bits_per_pixel = 24;

    format_list[2].fourcc = VA_FOURCC_P012;
    format_list[2].byte_order = VA_LSB_FIRST;
    format_list[2].bits_per_pixel = 24;

    *num_formats = 3;

    return VA_STATUS_SUCCESS;
}

VAStatus nvCreateImage(
        VADriverContextP ctx,
        VAImageFormat *format,
        int width,
        int height,
        VAImage *image     /* out */
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s\n", __FUNCTION__);

    Object imageObj = allocateObject(drv, OBJECT_TYPE_IMAGE, sizeof(NVImage));
    image->image_id = imageObj->id;

    LOG("created image id: %d\n", imageObj->id);

    NVImage *img = (NVImage*) imageObj->obj;
    img->width = width;
    img->height = height;
    img->format = format->fourcc;

    int bytesPerPixel = 1;
    if (format->fourcc == VA_FOURCC_P010 || format->fourcc == VA_FOURCC_P012) {
        bytesPerPixel = 2;
    }

    //allocate buffer to hold image when we copy down from the GPU
    //TODO could probably put these in a pool, they appear to be allocated, used, then freed
    Object imageBufferObject = allocateObject(drv, OBJECT_TYPE_BUFFER, sizeof(NVBuffer));
    NVBuffer *imageBuffer = (NVBuffer*) imageBufferObject->obj;
    imageBuffer->bufferType = VAImageBufferType;
    imageBuffer->size = (width * height + (width * height / 2)) * bytesPerPixel;
    imageBuffer->elements = 1;
    imageBuffer->ptr = aligned_alloc(16, imageBuffer->size);

    img->imageBuffer = imageBuffer;

    memcpy(&image->format, format, sizeof(VAImageFormat));
    image->buf = imageBufferObject->id;	/* image data buffer */
    /*
     * Image data will be stored in a buffer of type VAImageBufferType to facilitate
     * data store on the server side for optimal performance. The buffer will be
     * created by the CreateImage function, and proper storage allocated based on the image
     * size and format. This buffer is managed by the library implementation, and
     * accessed by the client through the buffer Map/Unmap functions.
     */
    image->width = width;
    image->height = height;
    image->data_size = imageBuffer->size;
    image->num_planes = 2;	/* can not be greater than 3 */
    /*
     * An array indicating the scanline pitch in bytes for each plane.
     * Each plane may have a different pitch. Maximum 3 planes for planar formats
     */
    image->pitches[0] = width * bytesPerPixel;
    image->pitches[1] = width * bytesPerPixel;
    /*
     * An array indicating the byte offset from the beginning of the image data
     * to the start of each plane.
     */
    image->offsets[0] = 0;
    image->offsets[1] = width * height * bytesPerPixel;

    return VA_STATUS_SUCCESS;
}

VAStatus nvDeriveImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImage *image     /* out */
    )
{
    LOG("In %s\n", __FUNCTION__);
    //FAILED because we don't support it yet
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

VAStatus nvDestroyImage(
        VADriverContextP ctx,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s\n", __FUNCTION__);

    NVImage *img = getObject(drv, image)->obj;

    Object imageBufferObj = getObjectByPtr(drv, img->imageBuffer);
    if (imageBufferObj != NULL) {
        deleteObject(drv, imageBufferObj->id);
    }

    deleteObject(drv, image);

    return VA_STATUS_SUCCESS;
}

VAStatus nvSetImagePalette(
            VADriverContextP ctx,
            VAImageID image,
            /*
                 * pointer to an array holding the palette data.  The size of the array is
                 * num_palette_entries * entry_bytes in size.  The order of the components
                 * in the palette is described by the component_order in VAImage struct
                 */
                unsigned char *palette
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvGetImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        int x,     /* coordinates of the upper left source pixel */
        int y,
        unsigned int width, /* width and height of the region */
        unsigned int height,
        VAImageID image
    )
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s\n", __FUNCTION__);

    NVSurface *surfaceObj = (NVSurface*) getObject(drv, surface)->obj;
    NVImage *imageObj = (NVImage*) getObject(drv, image)->obj;

    int bytesPerPixel = 1;
    if (imageObj->format == VA_FOURCC_P010 || imageObj->format == VA_FOURCC_P012) {
        bytesPerPixel = 2;
    }

    CUVIDPROCPARAMS procParams = {0};
    procParams.progressive_frame = surfaceObj->progressive_frame;
    procParams.top_field_first = surfaceObj->top_field_first;
    procParams.second_field = surfaceObj->second_field;

    CUdeviceptr deviceMemory = (CUdeviceptr) NULL;
    unsigned int pitch;

    CUresult result = cuvidMapVideoFrame(surfaceObj->decoder, surfaceObj->pictureIdx, &deviceMemory, &pitch, &procParams);
    LOG("got address %X for surface %d\n", deviceMemory, getObject(drv, surface)->id);

    if (result != CUDA_SUCCESS)
    {
            LOG("cuvidMapVideoFrame failed: %d\n", result);
            return VA_STATUS_ERROR_DECODING_ERROR;
    }

    CUDA_MEMCPY2D memcpy2d = {
      .srcXInBytes = 0, .srcY = 0,
      .srcMemoryType = CU_MEMORYTYPE_DEVICE,
      .srcDevice = deviceMemory,
      .srcPitch = pitch,

      .dstXInBytes = 0, .dstY = 0,
      .dstMemoryType = CU_MEMORYTYPE_HOST,
      .dstHost = imageObj->imageBuffer->ptr,
      .dstPitch = width * bytesPerPixel,

      .WidthInBytes = width * bytesPerPixel,
      .Height = height + (height>>1) //luma and chroma
    };

    result = cuMemcpy2D(&memcpy2d);
    if (result != CUDA_SUCCESS)
    {
            LOG("cuMemcpy2D failed: %d\n", result);
            return VA_STATUS_ERROR_DECODING_ERROR;
    }

    cuvidUnmapVideoFrame(surfaceObj->decoder, deviceMemory);

//    static int counter = 0;
//    char filename[64];
//    int size = (pitch * surfaceObj->height) + (pitch * surfaceObj->height>>1);
//    char *buf = malloc(size);
//    snLOG(filename, 64, "/tmp/frame-%03d.data\0", counter++);
//    LOG("writing %d to %s\n", surfaceObj->pictureIdx, filename);
//    int fd = open(filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
//    cuMemcpyDtoH(buf, deviceMemory, size);
//    write(fd, buf, size);
//    free(buf);
//    close(fd);

    return VA_STATUS_SUCCESS;
}

VAStatus nvPutImage(
        VADriverContextP ctx,
        VASurfaceID surface,
        VAImageID image,
        int src_x,
        int src_y,
        unsigned int src_width,
        unsigned int src_height,
        int dest_x,
        int dest_y,
        unsigned int dest_width,
        unsigned int dest_height
    )
{
    LOG("In %s\n", __FUNCTION__);

    return VA_STATUS_SUCCESS;
}

VAStatus nvQuerySubpictureFormats(
        VADriverContextP ctx,
        VAImageFormat *format_list,        /* out */
        unsigned int *flags,       /* out */
        unsigned int *num_formats  /* out */
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvCreateSubpicture(
        VADriverContextP ctx,
        VAImageID image,
        VASubpictureID *subpicture   /* out */
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvDestroySubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvSetSubpictureImage(
                VADriverContextP ctx,
                VASubpictureID subpicture,
                VAImageID image
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvSetSubpictureChromakey(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        unsigned int chromakey_min,
        unsigned int chromakey_max,
        unsigned int chromakey_mask
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvSetSubpictureGlobalAlpha(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        float global_alpha
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvAssociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces,
        short src_x, /* upper left offset in subpicture */
        short src_y,
        unsigned short src_width,
        unsigned short src_height,
        short dest_x, /* upper left offset in surface */
        short dest_y,
        unsigned short dest_width,
        unsigned short dest_height,
        /*
         * whether to enable chroma-keying or global-alpha
         * see VA_SUBPICTURE_XXX values
         */
        unsigned int flags
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvDeassociateSubpicture(
        VADriverContextP ctx,
        VASubpictureID subpicture,
        VASurfaceID *target_surfaces,
        int num_surfaces
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvQueryDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* out */
        int *num_attributes		/* out */
        )
{
    LOG("In %s\n", __FUNCTION__);
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

VAStatus nvGetDisplayAttributes(
        VADriverContextP ctx,
        VADisplayAttribute *attr_list,	/* in/out */
        int num_attributes
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvSetDisplayAttributes(
        VADriverContextP ctx,
                VADisplayAttribute *attr_list,
                int num_attributes
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvQuerySurfaceAttributes(
        VADriverContextP    ctx,
	    VAConfigID          config,
	    VASurfaceAttrib    *attrib_list,
	    unsigned int       *num_attribs
	)
{
    NVDriver *drv = (NVDriver*) ctx->pDriverData;
    LOG("In %s with %d\n", __FUNCTION__, *num_attribs);

    NVConfig *cfg = (NVConfig*) getObject(drv, config)->obj;

    if (attrib_list == NULL) {
            *num_attribs = 5;
    } else {
        CUVIDDECODECAPS videoDecodeCaps = {
            .eCodecType      = cfg->cudaCodec,
            .eChromaFormat   = cfg->chromaFormat,
            .nBitDepthMinus8 = cfg->bitDepth - 8
        };

        CUresult result = cuvidGetDecoderCaps(&videoDecodeCaps);
        if (result != CUDA_SUCCESS) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        attrib_list[0].type = VASurfaceAttribPixelFormat;
        attrib_list[0].flags = 0;
        attrib_list[0].value.type = VAGenericValueTypeInteger;

        if (cfg->chromaFormat == cudaVideoChromaFormat_420) {
            if (cfg->bitDepth == 10 && (videoDecodeCaps.nOutputFormatMask & 2)) {
                attrib_list[0].value.value.i = VA_FOURCC_P010;
            } else if (cfg->bitDepth == 12 && (videoDecodeCaps.nOutputFormatMask & 2)) {
                attrib_list[0].value.value.i = VA_FOURCC_P012;
            } else if (videoDecodeCaps.nOutputFormatMask & 1) {
                attrib_list[0].value.value.i = VA_FOURCC_NV12;
            }
        } else {
            //TODO not sure what pixel formats are needed for 422 and 444 formats
            LOG("Unknown chrome format: %d\n", cfg->chromaFormat);
            return VA_STATUS_ERROR_INVALID_CONFIG;
        }

        attrib_list[1].type = VASurfaceAttribMinWidth;
        attrib_list[1].flags = 0;
        attrib_list[1].value.type = VAGenericValueTypeInteger;
        attrib_list[1].value.value.i = videoDecodeCaps.nMinWidth;

        attrib_list[2].type = VASurfaceAttribMinHeight;
        attrib_list[2].flags = 0;
        attrib_list[2].value.type = VAGenericValueTypeInteger;
        attrib_list[2].value.value.i = videoDecodeCaps.nMinHeight;

        attrib_list[3].type = VASurfaceAttribMaxWidth;
        attrib_list[3].flags = 0;
        attrib_list[3].value.type = VAGenericValueTypeInteger;
        attrib_list[3].value.value.i = videoDecodeCaps.nMaxWidth;

        attrib_list[4].type = VASurfaceAttribMaxHeight;
        attrib_list[4].flags = 0;
        attrib_list[4].value.type = VAGenericValueTypeInteger;
        attrib_list[4].value.value.i = videoDecodeCaps.nMaxHeight;
    }

    return VA_STATUS_SUCCESS;
}

/* used by va trace */
VAStatus nvBufferInfo(
           VADriverContextP ctx,      /* in */
           VABufferID buf_id,         /* in */
           VABufferType *type,        /* out */
           unsigned int *size,        /* out */
           unsigned int *num_elements /* out */
)
{
LOG("In %s\n", __FUNCTION__);
*size=0;
*num_elements=0;

return VA_STATUS_SUCCESS;
}

VAStatus nvAcquireBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id,         /* in */
            VABufferInfo *      buf_info        /* in/out */
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvReleaseBufferHandle(
            VADriverContextP    ctx,
            VABufferID          buf_id          /* in */
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

//        /* lock/unlock surface for external access */
VAStatus nvLockSurface(
        VADriverContextP ctx,
        VASurfaceID surface,
        unsigned int *fourcc, /* out  for follow argument */
        unsigned int *luma_stride,
        unsigned int *chroma_u_stride,
        unsigned int *chroma_v_stride,
        unsigned int *luma_offset,
        unsigned int *chroma_u_offset,
        unsigned int *chroma_v_offset,
        unsigned int *buffer_name, /* if it is not NULL, assign the low lever
                                    * surface buffer name
                                    */
        void **buffer /* if it is not NULL, map the surface buffer for
                       * CPU access
                       */
)
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvUnlockSurface(
        VADriverContextP ctx,
                VASurfaceID surface
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

//        /* DEPRECATED */
//VAStatus nvGetSurfaceAttributes(
//            VADriverContextP    dpy,
//            VAConfigID          config,
//            VASurfaceAttrib    *attrib_list,
//            unsigned int        num_attribs
//        )
//{
//    LOG("In %s\n", __FUNCTION__);
//    return VA_STATUS_ERROR_UNIMPLEMENTED;
//}

VAStatus nvCreateMFContext(
            VADriverContextP ctx,
            VAMFContextID *mfe_context    /* out */
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvMFAddContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvMFReleaseContext(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID context
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvMFSubmit(
            VADriverContextP ctx,
            VAMFContextID mf_context,
            VAContextID *contexts,
            int num_contexts
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
VAStatus nvCreateBuffer2(
            VADriverContextP ctx,
            VAContextID context,                /* in */
            VABufferType type,                  /* in */
            unsigned int width,                 /* in */
            unsigned int height,                /* in */
            unsigned int *unit_size,            /* out */
            unsigned int *pitch,                /* out */
            VABufferID *buf_id                  /* out */
    )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus nvQueryProcessingRate(
            VADriverContextP ctx,               /* in */
            VAConfigID config_id,               /* in */
            VAProcessingRateParameter *proc_buf,/* in */
            unsigned int *processing_rate	/* out */
        )
{
    LOG("In %s\n", __FUNCTION__);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

void exportCudaPtr(EGLDisplay dpy, EGLStreamKHR stream, CUeglStreamConnection *conn, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch, int *fourcc, int *fds, int *offsets, int *strides, EGLuint64KHR *mods, int *bppOut) {
    static int numFramesPresented = 0;
    // If there is a frame presented before we check if consumer
    // is done with it using cuEGLStreamProducerReturnFrame.
    //LOG("outstanding frames: %d\n", numFramesPresented);
    CUeglFrame eglframe = {
        .frame = {
            .pArray = {0, 0, 0}
        }
    };
    while (numFramesPresented) {
      //LOG("waiting for returned frame\n");
      CUresult cuStatus = cuEGLStreamProducerReturnFrame(conn, &eglframe, NULL);
      if (cuStatus == CUDA_ERROR_LAUNCH_TIMEOUT) {
        //LOG("timeout with %d outstanding\n", numFramesPresented);
        break;
      } else if (cuStatus != CUDA_SUCCESS) {
        checkCudaErrors(cuStatus);
      } else {
        //LOG("returned frame %dx%d %p %p\n", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
        numFramesPresented--;
      }
    }

    int width = surface->width;
    int height = surface->height;

    //check if the frame size if different and release the arrays
    //TODO figure out how to get the EGLimage freed aswell
    if (eglframe.width != width && eglframe.height != height) {
        if (eglframe.frame.pArray[0] != NULL) {
            cuArrayDestroy(eglframe.frame.pArray[0]);
            eglframe.frame.pArray[0] = NULL;
        }
        if (eglframe.frame.pArray[1] != NULL) {
            cuArrayDestroy(eglframe.frame.pArray[1]);
            eglframe.frame.pArray[1] = NULL;
        }
    }
    eglframe.width = width;
    eglframe.height = height;
    eglframe.depth = 1;
    eglframe.pitch = pitch;
    eglframe.planeCount = 2;
    eglframe.numChannels = 1;
    eglframe.frameType = CU_EGL_FRAME_TYPE_ARRAY;

    int bpp = 1;
    if (surface->format == cudaVideoSurfaceFormat_NV12) {
        eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_YVU420_SEMIPLANAR;
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
    } else if (surface->format == cudaVideoSurfaceFormat_P016) {
        //TODO not working, produces this error in mpv:
        //EGL_BAD_MATCH error: In eglCreateImageKHR: requested LINUX_DRM_FORMAT is not supported
        //this error seems to be coming from the NVIDIA EGL driver
        //this might be caused by the DRM_FORMAT_*'s below
        if (surface->bitdepth == 10) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else if (surface->bitdepth == 12) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y12V12U12_420_SEMIPLANAR;
        } else {
            LOG("Unknown bitdepth\n");
        }
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT16;
        bpp = 2;
    }
    *bppOut = bpp;

    //TODO in theory this should work, but the application attempting to bind that texture gets the following error:
    //GL_INVALID_OPERATION error generated. <image> and <target> are incompatible
    //eglframe.frameType = CU_EGL_FRAME_TYPE_PITCH;
    //eglframe.frame.pPitch[0] = (void*) ptr;
    //eglframe.frame.pPitch[1] = (void*) ptr + (height*pitch);

    //reuse the arrays if we can
    //creating new arrays will cause a new EGLimage to be created and you'll eventually run out of resources
    if (eglframe.frame.pArray[0] == NULL) {
        CUDA_ARRAY3D_DESCRIPTOR arrDesc = {
            .Width = width,
            .Height = height,
            .Depth = 0,
            .NumChannels = 1,
            .Flags = 0,
            .Format = eglframe.cuFormat
        };
        checkCudaErrors(cuArray3DCreate(&eglframe.frame.pArray[0], &arrDesc));
    }
    if (eglframe.frame.pArray[1] == NULL) {
        CUDA_ARRAY3D_DESCRIPTOR arr2Desc = {
            .Width = width >> 1,
            .Height = height >> 1,
            .Depth = 0,
            .NumChannels = 2,
            .Flags = 0,
            .Format = eglframe.cuFormat
        };
        checkCudaErrors(cuArray3DCreate(&eglframe.frame.pArray[1], &arr2Desc));
    }
    CUDA_MEMCPY3D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = eglframe.frame.pArray[0],
        .Height = height,
        .WidthInBytes = width * bpp,
        .Depth = 1
    };
    checkCudaErrors(cuMemcpy3D(&cpy));
    CUDA_MEMCPY3D cpy2 = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcY = height,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = eglframe.frame.pArray[1],
        .Height = height >> 1,
        .WidthInBytes = (width >> 1) * 2 * bpp,
        .Depth = 1
    };
    checkCudaErrors(cuMemcpy3D(&cpy2));

    //LOG("Presenting frame %dx%d %p %p\n", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
    checkCudaErrors(cuEGLStreamProducerPresentFrame( conn, eglframe, NULL ));
    numFramesPresented++;

    while (1) {
        EGLenum event = 0;
        EGLAttrib aux = 0;
        EGLint eventRet = eglQueryStreamConsumerEventNV(dpy, stream, 0, &event, &aux);
        if (eventRet == EGL_TIMEOUT_EXPIRED_KHR) {
            break;
        }

        if (event == EGL_STREAM_IMAGE_ADD_NV) {
            EGLImage image = eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_STREAM_CONSUMER_IMAGE_NV, stream, NULL);
            LOG("Adding image from EGLStream, eglCreateImage: %p\n", image);
        } else if (event == EGL_STREAM_IMAGE_AVAILABLE_NV) {
            EGLImage img;
            //somehow we get here with the previous frame, not the next one
            if (!eglStreamAcquireImageNV(dpy, stream, &img, EGL_NO_SYNC_NV)) {
                LOG("eglStreamAcquireImageNV failed\n");
                exit(EXIT_FAILURE);
            }

            int planes = 0;
            if (!eglExportDMABUFImageQueryMESA(dpy, img, fourcc, &planes, mods)) {
                LOG("eglExportDMABUFImageQueryMESA failed\n");
                exit(EXIT_FAILURE);
            }

            LOG("eglExportDMABUFImageQueryMESA: %p %.4s (%x) planes:%d mods:%lx %lx\n", img, (char*)fourcc, *fourcc, planes, mods[0], mods[1]);

            EGLBoolean r = eglExportDMABUFImageMESA(dpy, img, fds, strides, offsets);

            if (!r) {
                LOG("Unable to export image\n");
            }
//            LOG("eglExportDMABUFImageMESA: %d %d %d %d %d\n", r, fds[0], fds[1], fds[2], fds[3]);
//            LOG("strides: %d %d %d %d\n", strides[0], strides[1], strides[2], strides[3]);
//            LOG("offsets: %d %d %d %d\n", offsets[0], offsets[1], offsets[2], offsets[3]);

            r = eglStreamReleaseImageNV(dpy, stream, img, EGL_NO_SYNC_NV);
            if (!r) {
                LOG("Unable to release image\n");
            }
        } else if (event == EGL_STREAM_IMAGE_REMOVE_NV) {
            eglDestroyImage(dpy, (EGLImage) aux);
            LOG("Removing image from EGLStream, eglDestroyImage: %p\n", (EGLImage) aux);
        } else {
            LOG("Unhandled event: %X\n", event);
        }
    }
}

VAStatus nvExportSurfaceHandle(
            VADriverContextP    ctx,
            VASurfaceID         surface_id,     /* in */
            uint32_t            mem_type,       /* in */
            uint32_t            flags,          /* in */
            void               *descriptor      /* out */
)
{
    //TODO check mem_type
    //TODO deal with flags

    //LOG("got %d %X %X\n", surface_id, mem_type, flags);
    NVDriver *drv = (NVDriver*) ctx->pDriverData;

    cuCtxSetCurrent(drv->g_oContext);

    NVSurface *surfaceObj = (NVSurface*) getObject(drv, surface_id)->obj;

    CUdeviceptr deviceMemory = (CUdeviceptr) NULL;
    unsigned int pitch = 0;

    if (surfaceObj->pictureIdx != -1) {
        CUVIDPROCPARAMS procParams = {0};
        procParams.progressive_frame = surfaceObj->progressive_frame;
        procParams.top_field_first = surfaceObj->top_field_first;
        procParams.second_field = surfaceObj->second_field;

        checkCudaErrors(cuvidMapVideoFrame(surfaceObj->decoder, surfaceObj->pictureIdx, &deviceMemory, &pitch, &procParams));
        LOG("got address %llX (%d) for surface %d (picIdx: %d)\n", deviceMemory, pitch, surface_id, surfaceObj->pictureIdx);
    } else {
        //TODO what do we do about exporting surfaces that were not used for decoding
        cuMemAlloc(&deviceMemory, 10 * 1024 * 1024);
        pitch = 1024;
    }

    uint32_t fourcc, bpp;

    int fds[4] = {0, 0, 0, 0};
    EGLint strides[4] = {0, 0, 0, 0}, offsets[4] = {0, 0, 0, 0};
    EGLuint64KHR mods[4] = {0, 0, 0, 0};
    exportCudaPtr(drv->eglDisplay, drv->eglStream, &drv->cuStreamConnection, deviceMemory, surfaceObj, pitch, &fourcc, fds, offsets, strides, mods, &bpp);

    //since we have to make a copy of the data anyway, we can unmap here
    if (surfaceObj->pictureIdx != -1) {
        cuvidUnmapVideoFrame(surfaceObj->decoder, deviceMemory);
    } else {
        cuMemFree(deviceMemory);
    }

    //TODO only support 420 images (either NV12, P010 or P012)
    VADRMPRIMESurfaceDescriptor *ptr = (VADRMPRIMESurfaceDescriptor*) descriptor;
    ptr->fourcc = fourcc;
    ptr->width = surfaceObj->width;
    ptr->height = surfaceObj->height;
    ptr->num_layers = 2;
    ptr->num_objects = 2;

    ptr->objects[0].fd = fds[0];
    ptr->objects[0].size = surfaceObj->width * surfaceObj->height * bpp;
    ptr->objects[0].drm_format_modifier = mods[0];

    ptr->objects[1].fd = fds[1];
    ptr->objects[1].size = surfaceObj->width * (surfaceObj->height >> 1) * bpp;
    ptr->objects[1].drm_format_modifier = mods[1];

    ptr->layers[0].drm_format = fourcc == DRM_FORMAT_NV12 ? DRM_FORMAT_R8 : DRM_FORMAT_R16;
    ptr->layers[0].num_planes = 1;
    ptr->layers[0].object_index[0] = 0;
    ptr->layers[0].offset[0] = offsets[0];
    ptr->layers[0].pitch[0] = strides[0];

    ptr->layers[1].drm_format = fourcc == DRM_FORMAT_NV12 ? DRM_FORMAT_RG88 : DRM_FORMAT_RG1616;
    ptr->layers[1].num_planes = 1;
    ptr->layers[1].object_index[0] = 1;
    ptr->layers[1].offset[0] = offsets[1];
    ptr->layers[1].pitch[0] = strides[1];

    return VA_STATUS_SUCCESS;
}

VAStatus __vaDriverInit_1_0(VADriverContextP ctx)
{
    NVDriver *drv = (NVDriver*) calloc(1, sizeof(NVDriver));
    ctx->pDriverData = drv;

    CUresult res = cuInit(0);
    //a 999 error here means install nvidia-modprobe
    checkCudaErrors(res);
    checkCudaErrors(cuCtxCreate(&drv->g_oContext, CU_CTX_SCHED_BLOCKING_SYNC, 0));

    ctx->max_profiles = MAX_PROFILES;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
    ctx->max_display_attributes = 1;
    ctx->max_image_formats = 3;
    ctx->max_subpic_formats = 1;

    ctx->str_vendor = "VA-API -> NVDEC driver";

    eglQueryStreamConsumerEventNV = (PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) eglGetProcAddress("eglQueryStreamConsumerEventNV");
    eglStreamReleaseImageNV = (PFNEGLSTREAMRELEASEIMAGENVPROC) eglGetProcAddress("eglStreamReleaseImageNV");
    eglStreamAcquireImageNV = (PFNEGLSTREAMACQUIREIMAGENVPROC) eglGetProcAddress("eglStreamAcquireImageNV");
    eglExportDMABUFImageMESA = (PFNEGLEXPORTDMABUFIMAGEMESAPROC) eglGetProcAddress("eglExportDMABUFImageMESA");
    eglExportDMABUFImageQueryMESA = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC) eglGetProcAddress("eglExportDMABUFImageQueryMESA");

    PFNEGLCREATESTREAMKHRPROC eglCreateStreamKHR = (PFNEGLCREATESTREAMKHRPROC) eglGetProcAddress("eglCreateStreamKHR");
    PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC eglStreamImageConsumerConnectNV = (PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) eglGetProcAddress("eglStreamImageConsumerConnectNV");

    drv->eglDisplay = eglGetDisplay(NULL);
    eglInitialize(drv->eglDisplay, NULL, NULL);
    EGLint stream_attrib_list[] = { EGL_STREAM_FIFO_LENGTH_KHR, 10, EGL_NONE };
    drv->eglStream = eglCreateStreamKHR(drv->eglDisplay, stream_attrib_list);
    EGLAttrib consumer_attrib_list[] = { EGL_NONE };
    eglStreamImageConsumerConnectNV(drv->eglDisplay, drv->eglStream, 0, 0, consumer_attrib_list);
    checkCudaErrors(cuEGLStreamProducerConnect(&drv->cuStreamConnection, drv->eglStream, 1024, 1024));

    PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");
    //setup debug logging
    EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};
    eglDebugMessageControlKHR(debug, debugAttribs);

#define VTABLE(ctx, func) ctx->vtable->va ## func = nv ## func

    VTABLE(ctx, Terminate);
    VTABLE(ctx, QueryConfigProfiles);
    VTABLE(ctx, QueryConfigEntrypoints);
    VTABLE(ctx, QueryConfigAttributes);
    VTABLE(ctx, CreateConfig);
    VTABLE(ctx, DestroyConfig);
    VTABLE(ctx, GetConfigAttributes);
    VTABLE(ctx, CreateSurfaces);
    VTABLE(ctx, CreateSurfaces2);
    VTABLE(ctx, DestroySurfaces);
    VTABLE(ctx, CreateContext);
    VTABLE(ctx, DestroyContext);
    VTABLE(ctx, CreateBuffer);
    VTABLE(ctx, BufferSetNumElements);
    VTABLE(ctx, MapBuffer);
    VTABLE(ctx, UnmapBuffer);
    VTABLE(ctx, DestroyBuffer);
    VTABLE(ctx, BeginPicture);
    VTABLE(ctx, RenderPicture);
    VTABLE(ctx, EndPicture);
    VTABLE(ctx, SyncSurface);
    VTABLE(ctx, QuerySurfaceStatus);
    VTABLE(ctx, PutSurface);
    VTABLE(ctx, QueryImageFormats);
    VTABLE(ctx, CreateImage);
    VTABLE(ctx, DeriveImage);
    VTABLE(ctx, DestroyImage);
    VTABLE(ctx, SetImagePalette);
    VTABLE(ctx, GetImage);
    VTABLE(ctx, PutImage);
    VTABLE(ctx, QuerySubpictureFormats);
    VTABLE(ctx, CreateSubpicture);
    VTABLE(ctx, DestroySubpicture);
    VTABLE(ctx, SetSubpictureImage);
    VTABLE(ctx, SetSubpictureChromakey);
    VTABLE(ctx, SetSubpictureGlobalAlpha);
    VTABLE(ctx, AssociateSubpicture);
    VTABLE(ctx, DeassociateSubpicture);
    VTABLE(ctx, QueryDisplayAttributes);
    VTABLE(ctx, GetDisplayAttributes);
    VTABLE(ctx, SetDisplayAttributes);
    VTABLE(ctx, QuerySurfaceAttributes);
    VTABLE(ctx, BufferInfo);
    VTABLE(ctx, AcquireBufferHandle);
    VTABLE(ctx, ReleaseBufferHandle);
    VTABLE(ctx, LockSurface);
    VTABLE(ctx, UnlockSurface);
    VTABLE(ctx, CreateMFContext);
    VTABLE(ctx, MFAddContext);
    VTABLE(ctx, MFReleaseContext);
    VTABLE(ctx, MFSubmit);
    VTABLE(ctx, CreateBuffer2);
    VTABLE(ctx, QueryProcessingRate);
    VTABLE(ctx, ExportSurfaceHandle);

    return VA_STATUS_SUCCESS;
}
