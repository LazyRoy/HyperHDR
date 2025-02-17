#include "webserver/WebServer.h"
#include "HyperhdrConfig.h"
#include "StaticFileServing.h"
#include "QtHttpServer.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QTcpServer>
#include <QDateTime>

// bonjour
#ifdef ENABLE_BONJOUR
	#include <bonjour/bonjourserviceregister.h>
#endif

// netUtil
#include <utils/NetOrigin.h>

WebServer::WebServer(const QJsonDocument& config, bool useSsl, QObject* parent)
	: QObject(parent)
	, _port(0)
	, _config(config)
	, _useSsl(useSsl)
	, _log(Logger::getInstance("WEBSERVER"))
	, _server()
{

}

WebServer::~WebServer()
{
	stop();
}

void WebServer::initServer()
{
	Info(_log, "Initialize Webserver");
	_server = new QtHttpServer(this);
	_server->setServerName(QStringLiteral("HyperHDR Webserver"));

	if (_useSsl)
	{
		_server->setUseSecure();
		WEBSERVER_DEFAULT_PORT = 8092;
	}

	connect(_server, &QtHttpServer::started, this, &WebServer::onServerStarted);
	connect(_server, &QtHttpServer::stopped, this, &WebServer::onServerStopped);
	connect(_server, &QtHttpServer::error, this, &WebServer::onServerError);

	// create StaticFileServing
	_staticFileServing = new StaticFileServing(this);
	connect(_server, &QtHttpServer::requestNeedsReply, _staticFileServing, &StaticFileServing::onRequestNeedsReply);

	// init
	handleSettingsUpdate(settings::type::WEBSERVER, _config);
}

void WebServer::onServerStarted(quint16 port)
{
	_inited = true;

	Info(_log, "Started on port %d name '%s'", port, _server->getServerName().toStdString().c_str());

#ifdef ENABLE_BONJOUR
	if (!_useSsl)
	{
		if (_serviceRegister == nullptr)
		{
			_serviceRegister = new BonjourServiceRegister(this, "_hyperhdr-http._tcp", port);
			_serviceRegister->registerService();
		}
		else if (_serviceRegister->getPort() != port)
		{
			delete _serviceRegister;
			_serviceRegister = new BonjourServiceRegister(this, "_hyperhdr-http._tcp", port);
			_serviceRegister->registerService();
		}
	}
#endif

	emit stateChange(true);
}

void WebServer::onServerStopped()
{
	Info(_log, "Stopped %s", _server->getServerName().toStdString().c_str());
	emit stateChange(false);
}

void WebServer::onServerError(QString msg)
{
	Error(_log, "%s", msg.toStdString().c_str());
}

bool WebServer::portAvailable(quint16& port, Logger* log)
{
	const quint16 prevPort = port;
	QTcpServer server;
	while (!server.listen(QHostAddress::Any, port))
	{
		Warning(log, "Port '%d' is already in use, will increment", port);
		port++;
	}
	server.close();
	if (port != prevPort)
	{
		Warning(log, "The requested Port '%d' was already in use, will use Port '%d' instead", prevPort, port);
		return false;
	}
	return true;
}

void WebServer::handleSettingsUpdate(settings::type type, const QJsonDocument& config)
{
	if (type == settings::type::WEBSERVER)
	{
		Info(_log, "Apply Webserver settings");
		const QJsonObject& obj = config.object();

		_baseUrl = obj["document_root"].toString(WEBSERVER_DEFAULT_PATH);


		if ((_baseUrl != ":/webconfig") && !_baseUrl.trimmed().isEmpty())
		{
			QFileInfo info(_baseUrl);
			if (!info.exists() || !info.isDir())
			{
				Error(_log, "document_root '%s' is invalid", _baseUrl.toUtf8().constData());
				_baseUrl = WEBSERVER_DEFAULT_PATH;
			}
		}
		else
			_baseUrl = WEBSERVER_DEFAULT_PATH;

		Info(_log, "Set document root to: %s", _baseUrl.toUtf8().constData());
		_staticFileServing->setBaseUrl(_baseUrl);

		// ssl different port
		quint16 newPort = _useSsl ? obj["sslPort"].toInt(WEBSERVER_DEFAULT_PORT) : obj["port"].toInt(WEBSERVER_DEFAULT_PORT);
		if (_port != newPort)
		{
			_port = newPort;
			stop();
		}

		// eval if the port is available, will be incremented if not
		if (!_server->isListening())
			portAvailable(_port, _log);

		// on ssl we want .key .cert and probably key password
		if (_useSsl)
		{
			QString keyPath = obj["keyPath"].toString(WEBSERVER_DEFAULT_KEY_PATH);
			QString crtPath = obj["crtPath"].toString(WEBSERVER_DEFAULT_CRT_PATH);

			QSslKey currKey = _server->getPrivateKey();
			QList<QSslCertificate> currCerts = _server->getCertificates();

			// check keyPath
			if ((keyPath != WEBSERVER_DEFAULT_KEY_PATH) && !keyPath.trimmed().isEmpty())
			{
				QFileInfo kinfo(keyPath);
				if (!kinfo.exists())
				{
					Error(_log, "No SSL key found at '%s' falling back to internal", keyPath.toUtf8().constData());
					keyPath = WEBSERVER_DEFAULT_KEY_PATH;
				}
			}
			else
				keyPath = WEBSERVER_DEFAULT_KEY_PATH;

			// check crtPath
			if ((crtPath != WEBSERVER_DEFAULT_CRT_PATH) && !crtPath.trimmed().isEmpty())
			{
				QFileInfo cinfo(crtPath);
				if (!cinfo.exists())
				{
					Error(_log, "No SSL certificate found at '%s' falling back to internal", crtPath.toUtf8().constData());
					crtPath = WEBSERVER_DEFAULT_CRT_PATH;
				}
			}
			else
				crtPath = WEBSERVER_DEFAULT_CRT_PATH;

			// load and verify crt
			QFile cfile(crtPath);
			cfile.open(QIODevice::ReadOnly);
			QList<QSslCertificate> validList;
			QList<QSslCertificate> cList = QSslCertificate::fromDevice(&cfile, QSsl::Pem);
			cfile.close();

			// Filter for valid certs
			for (const auto& entry : cList) {
				if (!entry.isNull() && QDateTime::currentDateTime().daysTo(entry.expiryDate()) > 0)
					validList.append(entry);
				else
					Error(_log, "The provided SSL certificate is invalid/not supported/reached expiry date ('%s')", crtPath.toUtf8().constData());
			}

			if (!validList.isEmpty()) {
				Info(_log, "Setup SSL certificate");
				_server->setCertificates(validList);
			}
			else {
				Error(_log, "No valid SSL certificate has been found ('%s'). Did you install OpenSSL?", crtPath.toUtf8().constData());
			}

			// load and verify key
			QFile kfile(keyPath);
			kfile.open(QIODevice::ReadOnly);
			// The key should be RSA enrcrypted and PEM format, optional the passPhrase
			QSslKey key(&kfile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey, obj["keyPassPhrase"].toString().toUtf8());
			kfile.close();

			if (key.isNull()) {
				Error(_log, "The provided SSL key is invalid or not supported use RSA encrypt and PEM format ('%s')", keyPath.toUtf8().constData());
			}
			else {
				Info(_log, "Setup private SSL key");
				_server->setPrivateKey(key);
			}
		}

		start();
		emit portChanged(_port);
	}
}

void WebServer::start()
{
	_server->start(_port);
}

void WebServer::stop()
{
	_server->stop();
}

void WebServer::setSSDPDescription(const QString& desc)
{
	_staticFileServing->setSSDPDescription(desc);
}
