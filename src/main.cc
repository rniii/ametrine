#include <QtNetwork>
#include <QtWidgets>

#ifdef Q_OS_LINUX
const QString OS_NAME = "linux";
#elif Q_OS_WIN
const QString OS_NAME = "windows";
#elif Q_OS_MACOS
const QString OS_NAME = "osx";
#else
const QString OS_NAME = "unknown";
#endif
#ifdef Q_PROCESSOR_X86_32
const QString OS_ARCH = "x86";
#else
const QString OS_ARCH = "unknown";
#endif

const QString USERNAME = "joebiden";
const QString PISTON_URL =
    "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";
const QString RESOURCES_ENDPOINT = "https://resources.download.minecraft.net/";
const QString LIBRARIES_ENDPOINT = "https://libraries.minecraft.net/";

QDir getDataDirectory() {
  return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QDir getCacheDirectory() {
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

bool checkRules(QJsonValue rules) {
  if (!rules.isArray())
    return true;

  for (QJsonValue rule : rules.toArray()) {
    auto os = rule["os"];
    bool match = !os["name"].isUndefined()   ? os["name"] == OS_NAME
                 : !os["arch"].isUndefined() ? os["arch"] == OS_ARCH
                                             : false;

    if ((rule["action"] == "allow" && !match) ||
        (rule["action"] == "disallow" && match)) {
      return false;
    }
  }

  return true;
}

class VersionManifest {
public:
  QString latestRelease;
  QString latestSnapshot;
  QMap<QString, QString> versionUrls;

  static VersionManifest fromJson(const QJsonDocument &data) {
    VersionManifest manifest;
    manifest.latestRelease = data["latest"]["release"].toString();
    manifest.latestSnapshot = data["latest"]["snapshot"].toString();
    for (QJsonValue version : data["versions"].toArray()) {
      manifest.versionUrls[version["id"].toString()] =
          version["url"].toString();
    }
    return manifest;
  }
};

class VersionInfo {
public:
  QString id;
  QString type;
  QString mainClass;
  QString assets;
  QString assetIndex;
  QString clientJar;
  quint16 jvmVersion; // java will definitely exhaust uint8 by 2030
  QStringList libraries;

  static VersionInfo fromJson(const QString &id, const QJsonDocument &data) {
    VersionInfo info;
    info.id = id;
    info.type = data["type"].toString();
    info.mainClass = data["mainClass"].toString();
    info.assets = data["assets"].toString();
    info.assetIndex = data["assetIndex"]["url"].toString();
    info.clientJar = data["downloads"]["client"]["url"].toString();
    info.jvmVersion = data["javaVersion"]["majorVersion"].toInt();
    for (QJsonValue lib : data["libraries"].toArray()) {
      if (!checkRules(lib["rules"]))
        continue;

      info.libraries << lib["downloads"]["artifact"]["path"].toString();
    }

    return info;
  }
};

class VersionManager : QObject {
public:
  explicit VersionManager();
  VersionManifest fetchManifest();
  VersionInfo fetchVersion(const VersionManifest &manifest, const QString &id);
  QJsonDocument fetchAssets(const VersionInfo &version);

private:
  QJsonDocument fetchJson(const QNetworkRequest &req);
  QJsonDocument fetchJson(const QUrl &url);

  QNetworkDiskCache *cache;
  QNetworkAccessManager *client;
};

VersionManager::VersionManager() {
  cache = new QNetworkDiskCache(this);
  cache->setCacheDirectory(getCacheDirectory().filePath("network"));

  client = new QNetworkAccessManager(this);
  client->setCache(cache);
}

VersionManifest VersionManager::fetchManifest() {
  return VersionManifest::fromJson(fetchJson(PISTON_URL));
}

VersionInfo VersionManager::fetchVersion(const VersionManifest &manifest,
                                         const QString &id) {
  auto request = QNetworkRequest(manifest.versionUrls[id]);
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                       QNetworkRequest::PreferCache);
  return VersionInfo::fromJson(id, fetchJson(request));
}

QJsonDocument VersionManager::fetchAssets(const VersionInfo &version) {
  auto request = QNetworkRequest(version.assetIndex);
  request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                       QNetworkRequest::PreferCache);
  return fetchJson(request);
}

QJsonDocument VersionManager::fetchJson(const QNetworkRequest &req) {
  auto reply = client->get(req);
  while (reply->isRunning()) {
    qApp->processEvents();
    reply->waitForReadyRead(-1);
  }
  return QJsonDocument::fromJson(reply->readAll());
}

QJsonDocument VersionManager::fetchJson(const QUrl &url) {
  return fetchJson(QNetworkRequest(url));
}

class Launcher : QObject {
public:
  Launcher(const VersionInfo &version, const QJsonDocument &assets);
  void launchGame();

private:
  void downloadFiles();
  void downloadAsset(const QString &url, const QString &path);
  void jvmArgs();
  void gameArgs();

  VersionInfo version;
  QJsonDocument assets;
  QMap<QNetworkReply *, QString> pending;
  QNetworkAccessManager *client;
  QStringList args;

  QDir dataDir;
  QString assetsDir;
  QString librariesDir;
  QString gameDir;
  QString versionDir;
  QString nativesDir;
};

Launcher::Launcher(const VersionInfo &version, const QJsonDocument &assets)
    : version(version), assets(assets) {
  dataDir = getDataDirectory();
  assetsDir = dataDir.filePath("assets");
  librariesDir = dataDir.filePath("libraries");
  gameDir = dataDir.filePath("instances/" + version.id + "/minecraft");
  versionDir = dataDir.filePath("versions/" + version.id);
  nativesDir = getCacheDirectory().filePath("natives");
}

void Launcher::launchGame() {
  QDir(nativesDir).mkpath(".");

  downloadFiles();

  jvmArgs();
  gameArgs();

  auto game = new QProcess;
  game->setProcessChannelMode(QProcess::ForwardedChannels);
  game->start("/usr/lib/jvm/java-21-openjdk/bin/java", args);
}

void Launcher::jvmArgs() {
  QString classpath;
  for (auto lib : version.libraries) {
    classpath += librariesDir + "/" + lib;
    classpath += QDir::listSeparator();
  }
  classpath += versionDir + "/client.jar";

#ifdef Q_OS_MACOS
  args << "-XstartOnFirstThread";
#endif
#ifdef Q_OS_WIN
  args << "-XX:HeapDumpPath=MojangTricksIntelDriversForPerformance_javaw.exe_"
          "minecraft.exe.heapdump";
#endif
#ifdef Q_PROCESSOR_X86_32
  args << "-Xss1M";
#endif
  args << "-Djava.library.path=" + nativesDir;
  args << "-Djna.tmpdir=" + nativesDir;
  args << "-Dorg.lwjgl.system.SharedLibraryExtractPath=" + nativesDir;
  args << "-Dio.netty.native.workdir=" + nativesDir;
  args << "-Dminecraft.launcher.brand=Ametrine";
  args << "-Dminecraft.launcher.version=0.1.0";
  // args << "-Dlog4j.configurationFile=${path}";
  args << "-cp" << classpath;
  args << version.mainClass;
}

void Launcher::gameArgs() {
  args << "--username" << USERNAME;
  args << "--version" << version.id;
  args << "--gameDir" << gameDir;
  args << "--assetsDir" << assetsDir;
  args << "--assetIndex" << version.assets;
  args << "--accessToken"
       << "";
  args << "--versionType" << version.type;
}

void Launcher::downloadFiles() {
  client = new QNetworkAccessManager(this);
  connect(client, &QNetworkAccessManager::finished, [&](QNetworkReply *reply) {
    auto path = pending.take(reply);
    QFileInfo(path).dir().mkpath(".");

    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(reply->readAll());
    file.close();

    reply->deleteLater();

    qDebug() << path;
    qDebug() << pending.size() << "assets left";
  });

  for (auto lib : version.libraries) {
    downloadAsset(LIBRARIES_ENDPOINT + lib, librariesDir + "/" + lib);
  }

  auto objects = assets["objects"].toObject();
  for (auto obj = objects.constBegin(); obj != objects.constEnd(); obj++) {
    auto hash = obj.value()["hash"].toString();
    auto entry = hash.left(2) + "/" + hash;
    downloadAsset(RESOURCES_ENDPOINT + entry, assetsDir + "/objects/" + entry);
  }

  downloadAsset(version.clientJar, versionDir + "/client.jar");

  QFile index(assetsDir + "/indexes/" + version.assets + ".json");
  QFileInfo(index).dir().mkpath(".");

  index.open(QIODevice::WriteOnly);
  index.write(assets.toJson(QJsonDocument::Compact));
  index.close();
}

void Launcher::downloadAsset(const QString &url, const QString &path) {
  if (QFileInfo(path).exists())
    return;

  auto reply = client->get(QNetworkRequest(url));
  pending[reply] = path;
}

class MainWindow : public QMainWindow {
public:
  explicit MainWindow();

private:
  void createVersionList();
};

MainWindow::MainWindow() { createVersionList(); }

void MainWindow::createVersionList() {
  setCursor(Qt::WaitCursor);
  auto manager = new VersionManager;
  auto manifest = manager->fetchManifest();
  unsetCursor();

  auto widget = new QWidget;
  setCentralWidget(widget);

  auto launch = new QPushButton("Launch");
  connect(launch, &QPushButton::pressed, [=]() {
    setCursor(Qt::WaitCursor);
    auto version = manager->fetchVersion(manifest, manifest.latestRelease);
    auto assets = manager->fetchAssets(version);
    unsetCursor();

    auto launcher = new Launcher(version, assets);
    launcher->launchGame();
  });

  auto layout = new QVBoxLayout(widget);
  layout->addWidget(new QLabel("Latest: " + manifest.latestRelease));
  layout->addWidget(launch);
}

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  QApplication::setApplicationName("ametrine");
  QApplication::setOrganizationDomain("rinici.de");

  MainWindow win;
  win.show();

  return app.exec();
}
