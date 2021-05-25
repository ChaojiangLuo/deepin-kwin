/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "egl_gbm_backend.h"
#include "basiceglsurfacetexture_internal.h"
#include "basiceglsurfacetexture_wayland.h"
// kwin
#include "composite.h"
#include "drm_backend.h"
#include "drm_buffer_gbm.h"
#include "drm_output.h"
#include "gbm_surface.h"
#include "logging.h"
#include "options.h"
#include "renderloop_p.h"
#include "screens.h"
#include "surfaceitem_wayland.h"
#include "drm_gpu.h"
#include "linux_dmabuf.h"
#include "dumb_swapchain.h"
#include "kwineglutils_p.h"
#include "shadowbuffer.h"
// kwin libs
#include <kwinglplatform.h>
#include <kwineglimagetexture.h>
// system
#include <gbm.h>
#include <unistd.h>
#include <errno.h>
#include <egl_dmabuf.h>
#include <drm_fourcc.h>
// kwayland server
#include "KWaylandServer/surface_interface.h"
#include "KWaylandServer/buffer_interface.h"
#include "KWaylandServer/linuxdmabuf_v1_interface.h"
#include "KWaylandServer/clientconnection.h"

namespace KWin
{

EglGbmBackend::EglGbmBackend(DrmBackend *drmBackend, DrmGpu *gpu)
    : AbstractEglDrmBackend(drmBackend, gpu)
{
}

EglGbmBackend::~EglGbmBackend()
{
    cleanup();
}

void EglGbmBackend::cleanupSurfaces()
{
    // shadow buffer needs context current for destruction
    makeCurrent();
    m_outputs.clear();
}

bool EglGbmBackend::initializeEgl()
{
    initClientExtensions();
    EGLDisplay display = m_gpu->eglDisplay();

    // Use eglGetPlatformDisplayEXT() to get the display pointer
    // if the implementation supports it.
    if (display == EGL_NO_DISPLAY) {
        const bool hasMesaGBM = hasClientExtension(QByteArrayLiteral("EGL_MESA_platform_gbm"));
        const bool hasKHRGBM = hasClientExtension(QByteArrayLiteral("EGL_KHR_platform_gbm"));
        const GLenum platform = hasMesaGBM ? EGL_PLATFORM_GBM_MESA : EGL_PLATFORM_GBM_KHR;

        if (!hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_base")) ||
                (!hasMesaGBM && !hasKHRGBM)) {
            setFailed("Missing one or more extensions between EGL_EXT_platform_base, "
                      "EGL_MESA_platform_gbm, EGL_KHR_platform_gbm");
            return false;
        }

        auto device = gbm_create_device(m_gpu->fd());
        if (!device) {
            setFailed("Could not create gbm device");
            return false;
        }
        m_gpu->setGbmDevice(device);

        display = eglGetPlatformDisplayEXT(platform, device, nullptr);
        m_gpu->setEglDisplay(display);
    }

    if (display == EGL_NO_DISPLAY) {
        return false;
    }
    setEglDisplay(display);
    return initEglAPI();
}

void EglGbmBackend::init()
{
    if (!initializeEgl()) {
        setFailed("Could not initialize egl");
        return;
    }

    if (!supportsSurfacelessContext()) {
        setFailed("EGL_KHR_surfaceless_context extension is unavailable!");
        return;
    }

    if (!initRenderingContext()) {
        setFailed("Could not initialize rendering context");
        return;
    }
    initBufferAge();
    // at the moment: no secondary GPU -> no OpenGL context!
    if (isPrimary()) {
        initKWinGL();
        initWayland();
    }
}

bool EglGbmBackend::initRenderingContext()
{
    initBufferConfigs();
    if (isPrimary()) {
        if (!createContext() || !makeCurrent()) {
            return false;
        }
    }
    const auto outputs = m_gpu->outputs();
    for (const auto &output : outputs) {
        addOutput(output);
    }
    return true;
}
bool EglGbmBackend::resetOutput(Output &output, DrmOutput *drmOutput)
{
    output.output = drmOutput;
    const QSize size = drmOutput->hardwareTransforms() ? drmOutput->pixelSize() :
                                                         drmOutput->modeSize();
    int flags = GBM_BO_USE_RENDERING;
    if (drmOutput->gpu() == m_gpu) {
        flags |= GBM_BO_USE_SCANOUT;
    } else {
        flags |= GBM_BO_USE_LINEAR;
    }
    auto gbmSurface = QSharedPointer<GbmSurface>::create(m_gpu, size, GBM_FORMAT_XRGB8888, flags);
    if (!gbmSurface) {
        qCCritical(KWIN_DRM) << "Creating GBM surface failed:" << strerror(errno);
        return false;
    }
    output.gbmSurface = gbmSurface;

    if (output.output->hardwareTransforms()) {
        output.shadowBuffer = nullptr;
    } else {
        makeContextCurrent(output);
        output.shadowBuffer = QSharedPointer<ShadowBuffer>::create(output.output->pixelSize());
        if (!output.shadowBuffer->isComplete()) {
            return false;
        }
    }
    return true;
}

bool EglGbmBackend::addOutput(DrmOutput *drmOutput)
{
    if (isPrimary()) {
        Output newOutput;
        if (resetOutput(newOutput, drmOutput)) {
            QVector<Output> &outputs = drmOutput->gpu() == m_gpu ? m_outputs : m_secondaryGpuOutputs;
            connect(drmOutput, &DrmOutput::modeChanged, this,
                [drmOutput, &outputs, this] {
                    auto it = std::find_if(outputs.begin(), outputs.end(),
                        [drmOutput] (const auto &output) {
                            return output.output == drmOutput;
                        }
                    );
                    if (it == outputs.end()) {
                        return;
                    }
                    resetOutput(*it, drmOutput);
                }
            );
            outputs << newOutput;
        } else {
            return false;
        }
    } else {
        Output newOutput;
        newOutput.output = drmOutput;
        if (!renderingBackend()->addOutput(drmOutput)) {
            return false;
        }
        m_outputs << newOutput;
    }
    return true;
}

void EglGbmBackend::removeOutput(DrmOutput *drmOutput)
{
    QVector<Output> &outputs = drmOutput->gpu() == m_gpu ? m_outputs : m_secondaryGpuOutputs;
    auto it = std::find_if(outputs.begin(), outputs.end(),
        [drmOutput] (const Output &output) {
            return output.output == drmOutput;
        }
    );
    if (it == outputs.end()) {
        return;
    }
    if (isPrimary()) {
        // shadow buffer needs context current for destruction
        makeCurrent();
    } else {
        renderingBackend()->removeOutput((*it).output);
    }
    outputs.erase(it);
}

bool EglGbmBackend::swapBuffers(DrmOutput *drmOutput)
{
    auto it = std::find_if(m_secondaryGpuOutputs.begin(), m_secondaryGpuOutputs.end(),
        [drmOutput] (const Output &output) {
            return output.output == drmOutput;
        }
    );
    if (it == m_secondaryGpuOutputs.end()) {
        return false;
    }
    renderFramebufferToSurface(*it);
    return it->gbmSurface->swapBuffers() != nullptr;
}

bool EglGbmBackend::exportFramebuffer(DrmOutput *drmOutput, void *data, const QSize &size, uint32_t stride)
{
    auto it = std::find_if(m_secondaryGpuOutputs.begin(), m_secondaryGpuOutputs.end(),
        [drmOutput] (const Output &output) {
            return output.output == drmOutput;
        }
    );
    if (it == m_secondaryGpuOutputs.end()) {
        return false;
    }
    auto bo = it->gbmSurface->currentBuffer();
    if (!bo->map(GBM_BO_TRANSFER_READ)) {
        return false;
    }
    if (stride != bo->stride()) {
        // shouldn't happen if formats are the same
        return false;
    }
    return memcpy(data, bo->mappedData(), size.height() * stride);
}

int EglGbmBackend::exportFramebufferAsDmabuf(DrmOutput *output, uint32_t *format, uint32_t *stride)
{
    DrmOutput *drmOutput = static_cast<DrmOutput*>(output);
    auto it = std::find_if(m_secondaryGpuOutputs.begin(), m_secondaryGpuOutputs.end(),
        [drmOutput] (const Output &output) {
            return output.output == drmOutput;
        }
    );
    if (it == m_secondaryGpuOutputs.end()) {
        return -1;
    }
    auto bo = it->gbmSurface->currentBuffer()->getBo();
    int fd = gbm_bo_get_fd(bo);
    if (fd == -1) {
        qCWarning(KWIN_DRM) << "failed to export gbm_bo as dma-buf:" << strerror(errno);
        return -1;
    }
    *format = gbm_bo_get_format(bo);
    *stride = gbm_bo_get_stride(bo);
    return fd;
}

QRegion EglGbmBackend::beginFrameForSecondaryGpu(DrmOutput *drmOutput)
{
    auto it = std::find_if(m_secondaryGpuOutputs.begin(), m_secondaryGpuOutputs.end(),
        [drmOutput] (const Output &output) {
            return output.output == drmOutput;
        }
    );
    if (it == m_secondaryGpuOutputs.end()) {
        return QRegion();
    }
    return prepareRenderingForOutput(*it);
}

void EglGbmBackend::importFramebuffer(Output &output) const
{
    if (!renderingBackend()->swapBuffers(output.output)) {
        qCWarning(KWIN_DRM) << "swapping buffers failed on output" << output.output;
        return;
    }
    output.buffer = nullptr;
    const auto size = output.output->modeSize();
    if (output.importMode == ImportMode::Dmabuf) {
        uint32_t stride = 0;
        uint32_t format = 0;
        int fd = renderingBackend()->exportFramebufferAsDmabuf(output.output, &format, &stride);
        if (fd != -1) {
            struct gbm_import_fd_data data = {};
            data.fd = fd;
            data.width = (uint32_t) size.width();
            data.height = (uint32_t) size.height();
            data.stride = stride;
            data.format = format;
            gbm_bo *importedBuffer = gbm_bo_import(m_gpu->gbmDevice(), GBM_BO_IMPORT_FD, &data, GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
            close(fd);
            if (importedBuffer) {
                auto buffer = QSharedPointer<DrmGbmBuffer>::create(m_gpu, importedBuffer, nullptr);
                if (buffer->bufferId() > 0) {
                    output.buffer = buffer;
                    return;
                }
            }
        }
        qCDebug(KWIN_DRM) << "import with dmabuf failed! Switching to CPU import on output" << output.output;
        output.importMode = ImportMode::DumbBuffer;
    }
    // ImportMode::DumbBuffer
    if (!output.importSwapchain || output.importSwapchain->size() != size) {
        output.importSwapchain = QSharedPointer<DumbSwapchain>::create(m_gpu, size);
        if (output.importSwapchain->isEmpty()) {
            output.importSwapchain = nullptr;
        }
    }
    if (output.importSwapchain) {
        auto buffer = output.importSwapchain->acquireBuffer();
        if (renderingBackend()->exportFramebuffer(output.output, buffer->data(), size, buffer->stride())) {
            output.buffer = buffer;
            return;
        }
    }
    qCWarning(KWIN_DRM) << "all imports failed on output" << output.output;
    // TODO turn off output?
}

void EglGbmBackend::renderFramebufferToSurface(Output &output)
{
    if (!output.shadowBuffer) {
        // No additional render target.
        return;
    }
    makeContextCurrent(output);
    output.shadowBuffer->render(output.output);
}

bool EglGbmBackend::makeContextCurrent(const Output &output) const
{
    Q_ASSERT(isPrimary());
    const auto surface = output.gbmSurface;
    if (!surface) {
        return false;
    }
    if (eglMakeCurrent(eglDisplay(), surface->eglSurface(), surface->eglSurface(), context()) == EGL_FALSE) {
        qCCritical(KWIN_DRM) << "eglMakeCurrent failed:" << getEglErrorString();
        return false;
    }
    return true;
}

bool EglGbmBackend::initBufferConfigs()
{
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,         EGL_WINDOW_BIT,
        EGL_RED_SIZE,             1,
        EGL_GREEN_SIZE,           1,
        EGL_BLUE_SIZE,            1,
        EGL_ALPHA_SIZE,           0,
        EGL_RENDERABLE_TYPE,      isOpenGLES() ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT,
        EGL_CONFIG_CAVEAT,        EGL_NONE,
        EGL_NONE,
    };

    EGLint count;
    EGLConfig configs[1024];
    if (!eglChooseConfig(eglDisplay(), config_attribs, configs,
                         sizeof(configs) / sizeof(EGLConfig),
                         &count)) {
        qCCritical(KWIN_DRM) << "eglChooseConfig failed:" << getEglErrorString();
        return false;
    }

    qCDebug(KWIN_DRM) << "EGL buffer configs count:" << count;

    // Loop through all configs, choosing the first one that has suitable format.
    for (EGLint i = 0; i < count; i++) {
        EGLint gbmFormat;
        // Query some configuration parameters, to show in debug log.
        eglGetConfigAttrib(eglDisplay(), configs[i], EGL_NATIVE_VISUAL_ID, &gbmFormat);

        if (KWIN_DRM().isDebugEnabled()) {
            // GBM formats are declared as FOURCC code (integer from ASCII chars, so use this fact).
            char gbmFormatStr[sizeof(EGLint) + 1] = {0};
            memcpy(gbmFormatStr, &gbmFormat, sizeof(EGLint));

            // Query number of bits for color channel.
            EGLint blueSize, redSize, greenSize, alphaSize;
            eglGetConfigAttrib(eglDisplay(), configs[i], EGL_RED_SIZE, &redSize);
            eglGetConfigAttrib(eglDisplay(), configs[i], EGL_GREEN_SIZE, &greenSize);
            eglGetConfigAttrib(eglDisplay(), configs[i], EGL_BLUE_SIZE, &blueSize);
            eglGetConfigAttrib(eglDisplay(), configs[i], EGL_ALPHA_SIZE, &alphaSize);
            qCDebug(KWIN_DRM) << "  EGL config #" << i << " has GBM FOURCC format:" << gbmFormatStr
                              << "; color sizes (RGBA order):"
                              << redSize << greenSize << blueSize << alphaSize;
        }

        if ((gbmFormat == GBM_FORMAT_XRGB8888) || (gbmFormat == GBM_FORMAT_ARGB8888)) {
            setConfig(configs[i]);
            return true;
        }
    }

    qCCritical(KWIN_DRM) << "Choosing EGL config did not return a suitable config. There were"
                         << count << "configs.";
    return false;
}

static QVector<EGLint> regionToRects(const QRegion &region, AbstractWaylandOutput *output)
{
    const int height = output->modeSize().height();

    const QMatrix4x4 matrix = DrmOutput::logicalToNativeMatrix(output->geometry(),
                                                               output->scale(),
                                                               output->transform());

    QVector<EGLint> rects;
    rects.reserve(region.rectCount() * 4);
    for (const QRect &_rect : region) {
        const QRect rect = matrix.mapRect(_rect);

        rects << rect.left();
        rects << height - (rect.y() + rect.height());
        rects << rect.width();
        rects << rect.height();
    }
    return rects;
}

void EglGbmBackend::aboutToStartPainting(int screenId, const QRegion &damagedRegion)
{
    Q_ASSERT_X(screenId != -1, "aboutToStartPainting", "not using per screen rendering");
    const Output &output = m_outputs.at(screenId);
    if (output.bufferAge > 0 && !damagedRegion.isEmpty() && supportsPartialUpdate()) {
        const QRegion region = damagedRegion & output.output->geometry();

        QVector<EGLint> rects = regionToRects(region, output.output);
        const bool correct = eglSetDamageRegionKHR(eglDisplay(), output.gbmSurface->eglSurface(),
                                                   rects.data(), rects.count()/4);
        if (!correct) {
            qCWarning(KWIN_DRM) << "eglSetDamageRegionKHR failed:" << getEglErrorString();
        }
    }
}

bool EglGbmBackend::presentOnOutput(Output &output, const QRegion &damagedRegion)
{
    if (isPrimary() && !directScanoutActive(output)) {
        output.buffer = output.gbmSurface->swapBuffersForDrm();
    } else if (!output.buffer) {
        qCDebug(KWIN_DRM) << "imported buffer does not exist!";
        return false;
    }

    Q_EMIT output.output->outputChange(damagedRegion);
    if (!output.output->present(output.buffer)) {
        return false;
    }

    if (supportsBufferAge()) {
        eglQuerySurface(eglDisplay(), output.gbmSurface->eglSurface(), EGL_BUFFER_AGE_EXT, &output.bufferAge);
    }
    return true;
}

bool EglGbmBackend::directScanoutActive(const Output &output)
{
    return output.surfaceInterface != nullptr;
}

PlatformSurfaceTexture *EglGbmBackend::createPlatformSurfaceTextureInternal(SurfacePixmapInternal *pixmap)
{
    return new BasicEGLSurfaceTextureInternal(this, pixmap);
}

PlatformSurfaceTexture *EglGbmBackend::createPlatformSurfaceTextureWayland(SurfacePixmapWayland *pixmap)
{
    return new BasicEGLSurfaceTextureWayland(this, pixmap);
}

void EglGbmBackend::setViewport(const Output &output) const
{
    const QSize size = output.output->pixelSize();
    glViewport(0, 0, size.width(), size.height());
}

QRegion EglGbmBackend::beginFrame(int screenId)
{
    Output &output = m_outputs[screenId];
    if (output.surfaceInterface) {
        qCDebug(KWIN_DRM) << "Direct scanout stopped on output" << output.output->name();
    }
    output.surfaceInterface = nullptr;
    if (isPrimary()) {
        return prepareRenderingForOutput(output);
    } else {
        return renderingBackend()->beginFrameForSecondaryGpu(output.output);
    }
}

QRegion EglGbmBackend::prepareRenderingForOutput(Output &output) const
{
    makeContextCurrent(output);
    if (output.shadowBuffer) {
        output.shadowBuffer->bind();
    }
    setViewport(output);

    if (supportsBufferAge()) {
        QRegion region;

        // Note: An age of zero means the buffer contents are undefined
        if (output.bufferAge > 0 && output.bufferAge <= output.damageHistory.count()) {
            for (int i = 0; i < output.bufferAge - 1; i++)
                region |= output.damageHistory[i];
        } else {
            region = output.output->geometry();
        }

        return region;
    }
    return output.output->geometry();
}

void EglGbmBackend::endFrame(int screenId, const QRegion &renderedRegion,
                             const QRegion &damagedRegion)
{
    Q_UNUSED(renderedRegion)

    Output &output = m_outputs[screenId];
    DrmOutput *drmOutput = output.output;

    if (isPrimary()) {
        renderFramebufferToSurface(output);
    } else {
        importFramebuffer(output);
    }

    const QRegion dirty = damagedRegion.intersected(output.output->geometry());
    if (!presentOnOutput(output, dirty)) {
        output.damageHistory.clear();
        RenderLoopPrivate *renderLoopPrivate = RenderLoopPrivate::get(drmOutput->renderLoop());
        renderLoopPrivate->notifyFrameFailed();
        return;
    }

    if (supportsBufferAge()) {
        if (output.damageHistory.count() > 10) {
            output.damageHistory.removeLast();
        }
        output.damageHistory.prepend(dirty);
    }
}

bool EglGbmBackend::scanout(int screenId, SurfaceItem *surfaceItem)
{
    SurfaceItemWayland *item = qobject_cast<SurfaceItemWayland *>(surfaceItem);
    if (!item) {
        return false;
    }

    KWaylandServer::SurfaceInterface *surface = item->surface();
    if (!surface || !surface->buffer() || !surface->buffer()->linuxDmabufBuffer()) {
        return false;
    }
    auto buffer = surface->buffer();
    Output &output = m_outputs[screenId];
    if (buffer->linuxDmabufBuffer()->size() != output.output->modeSize()) {
        return false;
    }
    EglDmabufBuffer *dmabuf = static_cast<EglDmabufBuffer*>(buffer->linuxDmabufBuffer());
    if (!dmabuf || !dmabuf->planes().count() ||
        !gbm_device_is_format_supported(m_gpu->gbmDevice(), dmabuf->format(), GBM_BO_USE_SCANOUT)) {
        return false;
    }
    gbm_bo *importedBuffer;
    if (dmabuf->planes()[0].modifier != DRM_FORMAT_MOD_INVALID
        || dmabuf->planes()[0].offset > 0
        || dmabuf->planes().size() > 1) {
        if (!m_gpu->addFB2ModifiersSupported()) {
            return false;
        }
        gbm_import_fd_modifier_data data = {};
        data.format = dmabuf->format();
        data.width = (uint32_t) dmabuf->size().width();
        data.height = (uint32_t) dmabuf->size().height();
        data.num_fds = dmabuf->planes().count();
        data.modifier = dmabuf->planes()[0].modifier;
        for (int i = 0; i < dmabuf->planes().count(); i++) {
            auto plane = dmabuf->planes()[i];
            data.fds[i] = plane.fd;
            data.offsets[i] = plane.offset;
            data.strides[i] = plane.stride;
        }
        importedBuffer = gbm_bo_import(m_gpu->gbmDevice(), GBM_BO_IMPORT_FD_MODIFIER, &data, GBM_BO_USE_SCANOUT);
    } else {
        auto plane = dmabuf->planes()[0];
        gbm_import_fd_data data = {};
        data.fd = plane.fd;
        data.width = (uint32_t) dmabuf->size().width();
        data.height = (uint32_t) dmabuf->size().height();
        data.stride = plane.stride;
        data.format = dmabuf->format();
        importedBuffer = gbm_bo_import(m_gpu->gbmDevice(), GBM_BO_IMPORT_FD, &data, GBM_BO_USE_SCANOUT);
    }
    if (!importedBuffer) {
        if (errno != EINVAL) {
            qCWarning(KWIN_DRM) << "Importing buffer for direct scanout failed:" << strerror(errno);
        }
        return false;
    }
    // damage tracking for screen casting
    QRegion damage;
    if (output.surfaceInterface == surface && buffer->size() == output.output->modeSize()) {
        QRegion trackedDamage = surfaceItem->damage();
        surfaceItem->resetDamage();
        for (const auto &rect : trackedDamage) {
            auto damageRect = QRect(rect);
            damageRect.translate(output.output->geometry().topLeft());
            damage |= damageRect;
        }
    } else {
        damage = output.output->geometry();
    }
    output.buffer = QSharedPointer<DrmGbmBuffer>::create(m_gpu, importedBuffer, buffer);
    auto oldSurface = output.surfaceInterface;
    output.surfaceInterface = surface;
    // ensure that a context is current like with normal presentation
    makeCurrent();
    if (presentOnOutput(output, damage)) {
        if (oldSurface != surface) {
            auto path = surface->client()->executablePath();
            qCDebug(KWIN_DRM).nospace() << "Direct scanout starting on output " << output.output->name() << " for application \"" << path << "\"";
        }
        return true;
    } else {
        output.surfaceInterface = nullptr;
        return false;
    }
}

QSharedPointer<GLTexture> EglGbmBackend::textureForOutput(AbstractOutput *abstractOutput) const
{
    auto itOutput = std::find_if(m_outputs.begin(), m_outputs.end(),
        [abstractOutput] (const auto &output) {
            return output.output == abstractOutput;
        }
    );
    if (itOutput == m_outputs.end()) {
        itOutput = std::find_if(m_secondaryGpuOutputs.begin(), m_secondaryGpuOutputs.end(),
            [abstractOutput] (const auto &output) {
                return output.output == abstractOutput;
            }
        );
        if (itOutput == m_secondaryGpuOutputs.end()) {
            return {};
        }
    }

    DrmOutput *drmOutput = itOutput->output;
    if (itOutput->shadowBuffer) {
        const auto glTexture = QSharedPointer<KWin::GLTexture>::create(itOutput->shadowBuffer->texture(), GL_RGBA8, drmOutput->pixelSize());
        glTexture->setYInverted(true);
        return glTexture;
    }

    auto gbmBuffer = dynamic_cast<GbmBuffer*>(itOutput->buffer.data());
    if (!gbmBuffer) {
        qCWarning(KWIN_DRM) << "Failed to record frame: Dumb buffer used for presentation!";
        return {};
    }
    EGLImageKHR image = eglCreateImageKHR(eglDisplay(), nullptr, EGL_NATIVE_PIXMAP_KHR, gbmBuffer->getBo(), nullptr);
    if (image == EGL_NO_IMAGE_KHR) {
        qCWarning(KWIN_DRM) << "Failed to record frame: Error creating EGLImageKHR - " << glGetError();
        return {};
    }

    return QSharedPointer<EGLImageTexture>::create(eglDisplay(), image, GL_RGBA8, drmOutput->modeSize());
}

bool EglGbmBackend::directScanoutAllowed(int screen) const
{
    return !m_backend->usesSoftwareCursor() && !m_outputs[screen].output->directScanoutInhibited();
}

}
