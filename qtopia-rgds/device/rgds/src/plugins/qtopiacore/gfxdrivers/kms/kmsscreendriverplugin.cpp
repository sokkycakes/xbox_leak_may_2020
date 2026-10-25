#include "kmsscreendriverplugin.h"
#include "kmsscreen.h"

#include <qtopiaglobal.h>

KmsScreenDriverPlugin::KmsScreenDriverPlugin( QObject *parent )
: QScreenDriverPlugin( parent )
{
}

KmsScreenDriverPlugin::~KmsScreenDriverPlugin()
{
}

QScreen* KmsScreenDriverPlugin::create(const QString& key, int displayId)
{
    if (key.toLower() == "kms")
        return new KmsScreen(displayId);

    return 0;
}

QStringList KmsScreenDriverPlugin::keys() const
{
    return QStringList() << "kms";
}

QTOPIA_EXPORT_QT_PLUGIN(KmsScreenDriverPlugin)
