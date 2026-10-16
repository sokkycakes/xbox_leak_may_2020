#include "kmsscreen.h"

#include <QApplication>
#include <QMap>
#include <QRegion>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QtDebug>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define KMS_SHARED_MAGIC 0x4b4d5331 // "KMS1"
#define KMS_HEADER_SIZE 4096

struct KmsShared
{
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t mmWidth;
    uint32_t mmHeight;
};

// Every screen of one device shares a single fd, so the server stays DRM master
// for all of them.
static QMap<QString, int> drmFds;
static QSet<uint32_t> claimedCrtcs;

static int openDevice(const QString &device)
{
    if (drmFds.contains(device))
        return drmFds.value(device);
    int fd = ::open(device.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if (fd >= 0)
        drmFds.insert(device, fd);
    return fd;
}

KmsScreen::KmsScreen(int displayId)
    : QScreen(displayId), drmFd(-1), shmId(-1), shared(0), scanout(0),
      scanoutSize(0), scanoutPitch(0), dumbHandle(0), fbId(0), crtcId(0),
      connectorId(0), savedCrtc(0), mode(0)
{
}

KmsScreen::~KmsScreen()
{
    delete static_cast<drmModeModeInfo *>(mode);
}

bool KmsScreen::openDrm(const QString &device, int connectorIndex)
{
    drmFd = openDevice(device);
    if (drmFd < 0) {
        qWarning("KmsScreen: cannot open %s: %s", device.toLocal8Bit().constData(), strerror(errno));
        return false;
    }

    drmModeRes *res = drmModeGetResources(drmFd);
    if (!res) {
        qWarning("KmsScreen: %s has no KMS resources", device.toLocal8Bit().constData());
        return false;
    }

    // Pick the Nth connected connector.
    drmModeConnector *conn = 0;
    int seen = 0;
    for (int i = 0; i < res->count_connectors && !conn; ++i) {
        drmModeConnector *c = drmModeGetConnector(drmFd, res->connectors[i]);
        if (!c)
            continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0 && seen++ == connectorIndex)
            conn = c;
        else
            drmModeFreeConnector(c);
    }
    if (!conn) {
        qWarning("KmsScreen: no connected connector #%d", connectorIndex);
        drmModeFreeResources(res);
        return false;
    }

    drmModeModeInfo *m = &conn->modes[0];
    for (int i = 0; i < conn->count_modes; ++i) {
        if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            m = &conn->modes[i];
            break;
        }
    }
    mode = new drmModeModeInfo(*m);
    connectorId = conn->connector_id;

    // Keep the CRTC already driving this connector, else claim a free one.
    crtcId = 0;
    if (conn->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(drmFd, conn->encoder_id);
        if (enc) {
            if (enc->crtc_id && !claimedCrtcs.contains(enc->crtc_id))
                crtcId = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    for (int e = 0; e < conn->count_encoders && !crtcId; ++e) {
        drmModeEncoder *enc = drmModeGetEncoder(drmFd, conn->encoders[e]);
        if (!enc)
            continue;
        for (int c = 0; c < res->count_crtcs; ++c) {
            if ((enc->possible_crtcs & (1 << c)) && !claimedCrtcs.contains(res->crtcs[c])) {
                crtcId = res->crtcs[c];
                break;
            }
        }
        drmModeFreeEncoder(enc);
    }
    if (!crtcId) {
        qWarning("KmsScreen: no free CRTC for connector %u", connectorId);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        return false;
    }
    claimedCrtcs.insert(crtcId);

    physWidth = conn->mmWidth;
    physHeight = conn->mmHeight;

    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    return true;
}

bool KmsScreen::connect(const QString &displaySpec)
{
    QString device = QLatin1String("/dev/dri/card0");
    int connectorIndex = displayId;
    int mmW = -1, mmH = -1;

    const QStringList args = displaySpec.split(QLatin1Char(':'));
    foreach (const QString &arg, args) {
        if (arg.startsWith(QLatin1String("dev=")))
            device = arg.mid(4);
        else if (arg.startsWith(QLatin1String("/dev/")))
            device = arg;
        else if (arg.startsWith(QLatin1String("connector=")))
            connectorIndex = arg.mid(10).toInt();
        else if (arg.startsWith(QLatin1String("mmWidth=")))
            mmW = arg.mid(8).toInt();
        else if (arg.startsWith(QLatin1String("mmHeight=")))
            mmH = arg.mid(9).toInt();
    }

    const bool server = QApplication::type() == QApplication::GuiServer;
    key_t key = ftok(device.toLocal8Bit().constData(), 'Q' + displayId);

    if (server) {
        if (!openDrm(device, connectorIndex))
            return false;
        drmModeModeInfo *m = static_cast<drmModeModeInfo *>(mode);
        const uint32_t pitch = m->hdisplay * 4;
        const size_t bytes = KMS_HEADER_SIZE + size_t(pitch) * m->vdisplay;

        // Drop a segment left behind by a crashed server.
        int stale = shmget(key, 0, 0);
        if (stale != -1)
            shmctl(stale, IPC_RMID, 0);
        shmId = shmget(key, bytes, IPC_CREAT | IPC_EXCL | 0666);
        if (shmId == -1) {
            qWarning("KmsScreen: shmget failed: %s", strerror(errno));
            return false;
        }
        shared = static_cast<KmsShared *>(shmat(shmId, 0, 0));
        if (shared == (void *)-1) {
            shared = 0;
            return false;
        }
        memset(shared, 0, bytes);
        shared->width = m->hdisplay;
        shared->height = m->vdisplay;
        shared->pitch = pitch;
        shared->mmWidth = physWidth;
        shared->mmHeight = physHeight;
        shared->magic = KMS_SHARED_MAGIC;
    } else {
        shmId = shmget(key, 0, 0);
        if (shmId == -1) {
            qWarning("KmsScreen: no server screen %d", displayId);
            return false;
        }
        shared = static_cast<KmsShared *>(shmat(shmId, 0, 0));
        if (shared == (void *)-1 || shared->magic != KMS_SHARED_MAGIC) {
            shared = 0;
            return false;
        }
    }

    data = reinterpret_cast<uchar *>(shared) + KMS_HEADER_SIZE;
    w = dw = shared->width;
    h = dh = shared->height;
    d = 32;
    lstep = shared->pitch;
    size = lstep * h;
    mapsize = size;
    screencols = 0;
    grayscale = false;
    setPixelFormat(QImage::Format_RGB32);

    // Panels that report no size get 96 dpi, unless the spec says otherwise.
    physWidth = mmW > 0 ? mmW : (shared->mmWidth ? int(shared->mmWidth) : qRound(w * 25.4 / 96));
    physHeight = mmH > 0 ? mmH : (shared->mmHeight ? int(shared->mmHeight) : qRound(h * 25.4 / 96));

    return true;
}

void KmsScreen::disconnect()
{
    if (shared) {
        shmdt(shared);
        shared = 0;
    }
    if (QApplication::type() == QApplication::GuiServer && shmId != -1)
        shmctl(shmId, IPC_RMID, 0);
    shmId = -1;
    data = 0;
}

bool KmsScreen::initDevice()
{
    drmModeModeInfo *m = static_cast<drmModeModeInfo *>(mode);
    if (drmFd < 0 || !m)
        return false;

    struct drm_mode_create_dumb create;
    memset(&create, 0, sizeof(create));
    create.width = w;
    create.height = h;
    create.bpp = 32;
    if (drmIoctl(drmFd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        qWarning("KmsScreen: cannot create dumb buffer: %s", strerror(errno));
        return false;
    }
    dumbHandle = create.handle;
    scanoutPitch = create.pitch;
    scanoutSize = create.size;

    if (drmModeAddFB(drmFd, w, h, 24, 32, scanoutPitch, dumbHandle, &fbId)) {
        qWarning("KmsScreen: cannot add framebuffer: %s", strerror(errno));
        return false;
    }

    struct drm_mode_map_dumb map;
    memset(&map, 0, sizeof(map));
    map.handle = dumbHandle;
    if (drmIoctl(drmFd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
        return false;
    void *p = mmap(0, scanoutSize, PROT_READ | PROT_WRITE, MAP_SHARED, drmFd, map.offset);
    if (p == MAP_FAILED)
        return false;
    scanout = static_cast<uchar *>(p);
    memset(scanout, 0, scanoutSize);

    savedCrtc = drmModeGetCrtc(drmFd, crtcId);
    if (drmModeSetCrtc(drmFd, crtcId, fbId, 0, 0, &connectorId, 1, m)) {
        qWarning("KmsScreen: cannot set mode %dx%d: %s", w, h, strerror(errno));
        return false;
    }

#ifndef QT_NO_QWS_CURSOR
    QScreenCursor::initSoftwareCursor();
#endif
    return true;
}

void KmsScreen::shutdownDevice()
{
    if (drmFd < 0)
        return;
    drmModeCrtc *saved = static_cast<drmModeCrtc *>(savedCrtc);
    if (saved) {
        drmModeSetCrtc(drmFd, saved->crtc_id, saved->buffer_id, saved->x, saved->y,
                       &connectorId, 1, saved->mode_valid ? &saved->mode : 0);
        drmModeFreeCrtc(saved);
        savedCrtc = 0;
    }
    if (scanout) {
        munmap(scanout, scanoutSize);
        scanout = 0;
    }
    if (fbId) {
        drmModeRmFB(drmFd, fbId);
        fbId = 0;
    }
    if (dumbHandle) {
        struct drm_mode_destroy_dumb destroy;
        memset(&destroy, 0, sizeof(destroy));
        destroy.handle = dumbHandle;
        drmIoctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        dumbHandle = 0;
    }
    claimedCrtcs.remove(crtcId);
}

void KmsScreen::setMode(int, int, int)
{
    // The panel mode is fixed; QWS asks for whatever connect() reported.
}

void KmsScreen::blank(bool on)
{
    if (drmFd < 0)
        return;
    drmModeConnector *conn = drmModeGetConnector(drmFd, connectorId);
    if (!conn)
        return;
    for (int i = 0; i < conn->count_props; ++i) {
        drmModePropertyRes *prop = drmModeGetProperty(drmFd, conn->props[i]);
        if (!prop)
            continue;
        if (!strcmp(prop->name, "DPMS"))
            drmModeConnectorSetProperty(drmFd, connectorId, prop->prop_id,
                                        on ? DRM_MODE_DPMS_OFF : DRM_MODE_DPMS_ON);
        drmModeFreeProperty(prop);
    }
    drmModeFreeConnector(conn);
}

// Regions arrive in global coordinates (a Multi screen hands each sub-screen
// its part), so flush() gets them moved to this screen's origin.
void KmsScreen::blit(const QImage &img, const QPoint &topLeft, const QRegion &region)
{
    QScreen::blit(img, topLeft, region);
    flush(region.translated(-offset()) & QRect(0, 0, w, h));
}

void KmsScreen::solidFill(const QColor &color, const QRegion &region)
{
    QScreen::solidFill(color, region);
    flush(region.translated(-offset()) & QRect(0, 0, w, h));
}

void KmsScreen::setDirty(const QRect &rect)
{
    QScreen::setDirty(rect);
    flush(QRegion(rect.translated(-offset())) & QRect(0, 0, w, h));
}

void KmsScreen::flush(const QRegion &region)
{
    if (!scanout || region.isEmpty())
        return;

    const QVector<QRect> rects = region.rects();
    QVector<drmModeClip> clips;
    clips.reserve(rects.size());
    foreach (const QRect &r, rects) {
        const int bytes = r.width() * 4;
        const uchar *src = data + r.y() * lstep + r.x() * 4;
        uchar *dst = scanout + r.y() * scanoutPitch + r.x() * 4;
        for (int y = 0; y < r.height(); ++y) {
            memcpy(dst, src, bytes);
            src += lstep;
            dst += scanoutPitch;
        }
        drmModeClip clip;
        clip.x1 = r.x();
        clip.y1 = r.y();
        clip.x2 = r.x() + r.width();
        clip.y2 = r.y() + r.height();
        clips.append(clip);
    }
    // Needed by virtual GPUs; scanout hardware like the RK3568 VOP2 ignores it.
    drmModeDirtyFB(drmFd, fbId, clips.data(), clips.size());
}
