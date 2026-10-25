#ifndef KMSSCREENDRIVERPLUGIN_H
#define KMSSCREENDRIVERPLUGIN_H

#include <QScreenDriverPlugin>

class KmsScreenDriverPlugin : public QScreenDriverPlugin {
    Q_OBJECT
public:
    KmsScreenDriverPlugin( QObject *parent  = 0 );
    ~KmsScreenDriverPlugin();

    QScreen* create(const QString& key, int displayId);
    QStringList keys() const;
};

#endif
