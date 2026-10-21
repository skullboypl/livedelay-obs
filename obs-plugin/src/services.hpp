#pragma once

#include <QString>
#include <QVector>

namespace livedelay {

// Platforms and their ingest servers, read from OBS's own list (rtmp-services
// services.json), so regions match what Settings → Stream offers.
struct Server {
	QString name, url;
};

struct Service {
	QString name;
	QVector<Server> servers;
	QString key_link;
	bool common = false;
};

// Common services first, then the rest by name. RTMP/RTMPS servers only.
const QVector<Service> &Services();
const Service *FindService(const QString &name);
// The service whose server list contains `url`, if any.
const Service *ServiceForUrl(const QString &url);

} // namespace livedelay
