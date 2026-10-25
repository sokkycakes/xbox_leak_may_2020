#ifndef KMSSCREEN_H
#define KMSSCREEN_H

#include <QScreen>

struct KmsShared;

// QWS screen on a DRM/KMS connector, no fbdev needed.
// Display spec: kms[:dev=/dev/dri/card0][:connector=N][:mmWidth=W][:mmHeight=H]
// Every process maps a shared shadow buffer; the server copies the regions
// draws (blit, solidFill, direct painting) into a dumb buffer scanned out by
// the CRTC.
class KmsScreen : public QScreen
{
public:
    explicit KmsScreen(int displayId);
    ~KmsScreen();

    bool connect(const QString &displaySpec);
    void disconnect();
    bool initDevice();
    void shutdownDevice();
    void setMode(int, int, int);
    void blank(bool on);
    void blit(const QImage &img, const QPoint &topLeft, const QRegion &region);
    void solidFill(const QColor &color, const QRegion &region);
    void setDirty(const QRect &rect);

private:
    bool openDrm(const QString &device, int connectorIndex);
    void flush(const QRegion &region);

    int drmFd;
    int shmId;
    KmsShared *shared;
    uchar *scanout;
    unsigned int scanoutSize;
    unsigned int scanoutPitch;
    unsigned int dumbHandle;
    unsigned int fbId;
    unsigned int crtcId;
    unsigned int connectorId;
    void *savedCrtc;
    void *mode;
};

#endif
