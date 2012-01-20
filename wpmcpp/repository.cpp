// this include should be before all the others or gcc shows errors
#include <xapian.h>

#include <windows.h>
#include <shlobj.h>

#include <QCryptographicHash>
#include "qtemporaryfile.h"
#include "downloader.h"
#include "qsettings.h"
#include "qdom.h"
#include "qdebug.h"
#include "QSet"

#include "repository.h"
#include "downloader.h"
#include "packageversionfile.h"
#include "wpmutils.h"
#include "version.h"
#include "msi.h"
#include "windowsregistry.h"
#include "xmlutils.h"
#include "packageversionhandle.h"

Repository Repository::def;

Repository::Repository(): AbstractRepository(), stemmer("english")
{
    this->db = 0;
    this->enquire = 0;
    this->queryParser = 0;
    addWellKnownPackages();

    indexer.set_stemmer(stemmer);
}

void Repository::index(Job* job)
{
    try {
        for (int i = 0; i < getPackageCount(); i++){
            Package* p = this->packages.at(i);

            Xapian::Document doc;
            this->indexCreateDocument(p, doc);

            indexer.set_document(doc);
            indexer.index_text(doc.get_data());

            // Add the document to the database.
            db->add_document(doc);

            if (i % 100 == 0) {
                job->setProgress(0.45 * i / getPackageCount());
                job->setHint(QString("indexing packages (%L1)").arg(i));
            }

            if (job->isCancelled())
                break;
        }

        for (int i = 0; i < getPackageVersionCount(); i++){
            PackageVersion* pv = this->getPackageVersion(i);

            Xapian::Document doc;
            this->indexCreateDocument(pv, doc);

            indexer.set_document(doc);
            indexer.index_text(doc.get_data());

            // Add the document to the database.
            db->add_document(doc);

            if (i % 100 == 0) {
                job->setProgress(0.45 + 0.45 * i / getPackageVersionCount());
                job->setHint(QString("indexing packages (%L1)").arg(i));
            }

            if (job->isCancelled())
                break;
        }

        // Explicitly commit so that we get to see any errors.  WritableDatabase's
        // destructor will commit implicitly (unless we're in a transaction) but
        // will swallow any exceptions produced.
        job->setHint("preparing the index");
        db->commit();

        if (!job->isCancelled())
            job->setProgress(1);

        job->complete();
    } catch (const Xapian::Error &e) {
        job->setErrorMessage(WPMUtils::fromUtf8StdString(
                e.get_description()));
    }
}

QList<Package*> Repository::find(const QString& text, int type,
        QString* warning)
{
    QList<Package*> r;
    QString t = text.trimmed();

    try {
        Xapian::Query query("Tpackage");

        if (!t.isEmpty()) {
            query = Xapian::Query(Xapian::Query::OP_AND, query,
                    queryParser->parse_query(
                    t.toUtf8().constData(),
                    Xapian::QueryParser::FLAG_PHRASE |
                    Xapian::QueryParser::FLAG_BOOLEAN |
                    Xapian::QueryParser::FLAG_LOVEHATE |
                    Xapian::QueryParser::FLAG_WILDCARD |
                    Xapian::QueryParser::FLAG_PARTIAL
            ));
        }

        switch (type) {
            case 1: // installed
                query = Xapian::Query(Xapian::Query::OP_AND, query,
                        Xapian::Query("Sinstalled"));
                break;
            case 2:  // installed, updateable
                query = Xapian::Query(Xapian::Query::OP_AND, query,
                        Xapian::Query("Sinstalled"));
                query = Xapian::Query(Xapian::Query::OP_AND, query,
                        Xapian::Query("Supdateable"));
                break;
        }

        enquire->set_query(query);
        const unsigned int max = 2000;
        Xapian::MSet matches = enquire->get_mset(0, max);
        if (matches.size() == max)
            *warning = QString(
                    "Only the first %L1 matches of about %L2 are shown").
                    arg(max).
                    arg(matches.get_matches_estimated());

        Xapian::MSetIterator i;
        for (i = matches.begin(); i != matches.end(); ++i) {
            Xapian::Document doc = i.get_document();
            std::string package = doc.get_value(0);
            QString package_ = WPMUtils::fromUtf8StdString(package);
            Package* p = this->findPackage(package_);
            if (p)
                r.append(p);
        }
    } catch (const Xapian::Error &e) {
        *warning = WPMUtils::fromUtf8StdString(e.get_description());
    }

    return r;
}

QList<PackageVersion*> Repository::getInstalled()
{
    QList<PackageVersion*> ret;

    for (int i = 0; i < getPackageVersionCount(); i++) {
        PackageVersion* pv = getPackageVersion(i);
        if (pv->installed()) {
            ret.append(pv);
        }
    }

    return ret;
}

bool Repository::isLocked(const QString& package, const Version& version) const
{
    bool result = false;
    for (int i = 0; i < this->locked.count(); i++) {
        PackageVersionHandle* pvh = this->locked.at(i);
        if (pvh->package == package && pvh->version == version) {
            result = true;
            break;
        }
    }
    return result;
}

void Repository::lock(const QString& package, const Version& version)
{
    this->locked.append(new PackageVersionHandle(package, version));
}

void Repository::unlock(const QString& package, const Version& version)
{
    for (int i = 0; i < this->locked.count(); i++) {
        PackageVersionHandle* pvh = this->locked.at(i);
        if (pvh->package == package && pvh->version == version) {
            delete this->locked.takeAt(i);
            break;
        }
    }
}

Repository::~Repository()
{
    delete queryParser;
    delete enquire;
    delete db;

    qDeleteAll(this->packages);
    qDeleteAll(this->packageVersions);
    qDeleteAll(this->licenses);
    qDeleteAll(this->installedPackageVersions);
    qDeleteAll(this->locked);
}

PackageVersion* Repository::findNewestInstallablePackageVersion(
        const QString &package)
{
    PackageVersion* r = 0;

    QList<PackageVersion*> list = this->getPackageVersions(package);
    for (int i = 0; i < list.count(); i++) {
        PackageVersion* p = list.at(i);
        if (r == 0 || p->version.compare(r->version) > 0) {
            if (p->download.isValid())
                r = p;
        }
    }
    return r;
}

PackageVersion* Repository::findNewestInstalledPackageVersion(
        const QString &name)
{
    PackageVersion* r = 0;

    QList<PackageVersion*> list = this->getPackageVersions(name);
    for (int i = 0; i < list.count(); i++) {
        PackageVersion* p = list.at(i);
        if (p->installed()) {
            if (r == 0 || p->version.compare(r->version) > 0) {
                r = p;
            }
        }
    }
    return r;
}

Package* Repository::createPackage(QDomElement* e, QString* err)
{
    *err = "";

    QString name = e->attribute("name").trimmed();
    if (name.isEmpty()) {
        err->append("Empty attribute 'name' in <package>)");
    }

    Package* a = new Package(name, name);

    if (err->isEmpty()) {
        a->title = XMLUtils::getTagContent(*e, "title");
        a->url = XMLUtils::getTagContent(*e, "url");
        a->description = XMLUtils::getTagContent(*e, "description");
    }

    if (err->isEmpty()) {
        a->icon = XMLUtils::getTagContent(*e, "icon");
        if (!a->icon.isEmpty()) {
            QUrl u(a->icon);
            if (!u.isValid() || u.isRelative() ||
                    !(u.scheme() == "http" || u.scheme() == "https")) {
                err->append(QString("Invalid icon URL for %1: %2").
                        arg(a->title).arg(a->icon));
            }
        }
    }

    if (err->isEmpty()) {
        a->license = XMLUtils::getTagContent(*e, "license");
    }

    if (err->isEmpty())
        return a;
    else {
        delete a;
        return 0;
    }
}

License* Repository::createLicense(QDomElement* e)
{
    QString name = e->attribute("name");
    License* a = new License(name, name);
    QDomNodeList nl = e->elementsByTagName("title");
    if (nl.count() != 0)
        a->title = nl.at(0).firstChild().nodeValue();
    nl = e->elementsByTagName("url");
    if (nl.count() != 0)
        a->url = nl.at(0).firstChild().nodeValue();
    nl = e->elementsByTagName("description");
    if (nl.count() != 0)
        a->description = nl.at(0).firstChild().nodeValue();

    return a;
}

License* Repository::findLicense(const QString& name)
{
    for (int i = 0; i < this->licenses.count(); i++) {
        if (this->licenses.at(i)->name == name)
            return this->licenses.at(i);
    }
    return 0;
}

QList<Package*> Repository::findPackages(const QString& name)
{
    QList<Package*> r;
    bool shortName = name.indexOf('.') < 0;
    QString suffix = '.' + name;
    for (int i = 0; i < this->getPackageCount(); i++) {
        Package* p = this->packages.at(i);
        if (p->name == name) {
            r.append(p);
        } else if (shortName && p->name.endsWith(suffix)) {
            r.append(p);
        }
    }
    return r;
}

Package* Repository::findPackage(const QString& name) const
{
    return this->nameToPackage.value(name);
}

int Repository::countUpdates()
{
    int r = 0;
    for (int i = 0; i < this->getPackageVersionCount(); i++) {
        PackageVersion* p = this->getPackageVersion(i);
        if (p->installed()) {
            PackageVersion* newest = findNewestInstallablePackageVersion(
                    p->getPackage()->name);
            if (newest->version.compare(p->version) > 0 && !newest->installed())
                r++;
        }
    }
    return r;
}

void Repository::indexCreateDocument(Package* p, Xapian::Document& doc)
{
    QString t = p->getFullText();
    std::string para = t.toUtf8().constData();
    doc.set_data(para);

    doc.add_value(0, p->name.toUtf8().constData());
    doc.add_boolean_term("Tpackage");

    boolean installed = false, updateable = false;
    for (int i = 0; i < this->installedPackageVersions.count(); i++) {
        InstalledPackageVersion* ipv = this->installedPackageVersions.at(i);
        if (ipv->package_ == p) {
            installed = true;
            PackageVersion* pv = findPackageVersion(ipv->package_->name,
                    ipv->version);
            if (pv && pv->isUpdateEnabled()) {
                updateable = true;
                break;
            }
        }
    }
    if (installed)
        doc.add_boolean_term("Sinstalled");
    if (updateable)
        doc.add_boolean_term("Supdateable");
}

void Repository::indexCreateDocument(PackageVersion* pv, Xapian::Document& doc)
{
    QString t = pv->getFullText();
    Package* p = pv->getPackage();
    if (p) {
        t += " ";
        t += p->getFullText();
    }

    std::string para = t.toUtf8().constData();
    doc.set_data(para);

    doc.add_value(0, pv->getPackage()->name.toUtf8().constData());
    doc.add_value(1, pv->version.getVersionString().
            toUtf8().constData());
    doc.add_value(2, pv->serialize().
            toUtf8().constData());

    doc.add_boolean_term("Tpackage_version");

    if (this->findInstalledPackageVersion(pv)) {
        doc.add_boolean_term("Sinstalled");
        if (pv->isUpdateEnabled())
            doc.add_boolean_term("Supdateable");
    } else {
        doc.add_boolean_term("Snot_installed");
    }

    /*
        bool installed = pv->installed();
        bool updateEnabled = isUpdateEnabled(pv);
        PackageVersion* newest = r->findNewestInstallablePackageVersion(
                pv->getPackage()->name);
        bool statusOK;
        switch (statusFilter) {
            case 0:
                // all
                statusOK = true;
                break;
            case 1:
                // not installed
                statusOK = !installed;
                break;
            case 2:
                // installed
                statusOK = installed;
                break;
            case 3:
                // installed, updateable
                statusOK = installed && updateEnabled;
                break;
            case 4:
                // newest or installed
                statusOK = installed || pv == newest;
                break;
            default:
                statusOK = true;
                break;
        }
        if (!statusOK)
            continue;

*/
}

QString Repository::indexUpdatePackageVersion(PackageVersion* pv)
{
    QString err;
    try {
        Xapian::Query query(Xapian::Query::OP_AND,
                Xapian::Query("Tpackage_version"),
                Xapian::Query("Snot_installed"));

        enquire->set_query(query);
        Xapian::MSet matches = enquire->get_mset(0, 1);
        if (matches.size() != 0) {
            db->delete_document(*matches.begin());
        }

        Xapian::Document doc;
        this->indexCreateDocument(pv, doc);

        indexer.set_document(doc);
        indexer.index_text(doc.get_data());

        // Add the document to the database.
        db->add_document(doc);

        db->commit();
    } catch (const Xapian::Error &e) {
        err = WPMUtils::fromUtf8StdString(e.get_description());
    }
    return err;
}

void Repository::addWellKnownPackages()
{
    if (!this->findPackage("com.microsoft.Windows")) {
        Package* p = new Package("com.microsoft.Windows", "Windows");
        p->url = "http://www.microsoft.com/windows/";
        p->description = "Operating system";
        addPackage(p);
    }
    if (!this->findPackage("com.microsoft.Windows32")) {
        Package* p = new Package("com.microsoft.Windows32", "Windows/32 bit");
        p->url = "http://www.microsoft.com/windows/";
        p->description = "Operating system";
        addPackage(p);
    }
    if (!this->findPackage("com.microsoft.Windows64")) {
        Package* p = new Package("com.microsoft.Windows64", "Windows/64 bit");
        p->url = "http://www.microsoft.com/windows/";
        p->description = "Operating system";
        addPackage(p);
    }
    if (!findPackage("com.googlecode.windows-package-manager.Npackd")) {
        Package* p = new Package("com.googlecode.windows-package-manager.Npackd",
                "Npackd");
        p->url = "http://code.google.com/p/windows-package-manager/";
        p->description = "package manager";
        addPackage(p);
    }
    if (!this->findPackage("com.oracle.JRE")) {
        Package* p = new Package("com.oracle.JRE", "JRE");
        p->url = "http://www.java.com/";
        p->description = "Java runtime";
        addPackage(p);
    }
    if (!this->findPackage("com.oracle.JRE64")) {
        Package* p = new Package("com.oracle.JRE64", "JRE/64 bit");
        p->url = "http://www.java.com/";
        p->description = "Java runtime";
        addPackage(p);
    }
    if (!this->findPackage("com.oracle.JDK")) {
        Package* p = new Package("com.oracle.JDK", "JDK");
        p->url = "http://www.oracle.com/technetwork/java/javase/overview/index.html";
        p->description = "Java development kit";
        addPackage(p);
    }
    if (!this->findPackage("com.oracle.JDK64")) {
        Package* p = new Package("com.oracle.JDK64", "JDK/64 bit");
        p->url = "http://www.oracle.com/technetwork/java/javase/overview/index.html";
        p->description = "Java development kit";
        addPackage(p);
    }
    if (!this->findPackage("com.microsoft.DotNetRedistributable")) {
        Package* p = new Package("com.microsoft.DotNetRedistributable",
                ".NET redistributable runtime");
        p->url = "http://msdn.microsoft.com/en-us/netframework/default.aspx";
        p->description = ".NET runtime";
        addPackage(p);
    }
    if (!this->findPackage("com.microsoft.WindowsInstaller")) {
        Package* p = new Package("com.microsoft.WindowsInstaller",
                "Windows Installer");
        p->url = "http://msdn.microsoft.com/en-us/library/cc185688(VS.85).aspx";
        p->description = "Package manager";
        addPackage(p);
    }
    if (!this->findPackage("com.microsoft.MSXML")) {
        Package* p = new Package("com.microsoft.MSXML",
                "Microsoft Core XML Services (MSXML)");
        p->url = "http://www.microsoft.com/downloads/en/details.aspx?FamilyID=993c0bcf-3bcf-4009-be21-27e85e1857b1#Overview";
        p->description = "XML library";
        addPackage(p);
    }
}

QString Repository::planUpdates(const QList<Package*> packages,
        QList<InstallOperation*>& ops)
{
    QList<PackageVersion*> installed = getInstalled();
    QList<PackageVersion*> newest, newesti;
    QList<bool> used;

    QString err;

    for (int i = 0; i < packages.count(); i++) {
        Package* p = packages.at(i);

        PackageVersion* a = findNewestInstallablePackageVersion(p->name);
        if (a == 0) {
            err = QString("No installable version found for the package %1").
                    arg(p->title);
            break;
        }

        PackageVersion* b = findNewestInstalledPackageVersion(p->name);
        if (b == 0) {
            err = QString("No installed version found for the package %1").
                    arg(p->title);
            break;
        }

        if (a->version.compare(b->version) <= 0) {
            err = QString("The newest version (%1) for the package %2 is already installed").
                    arg(b->version.getVersionString()).arg(p->title);
            break;
        }

        newest.append(a);
        newesti.append(b);
        used.append(false);
    }

    if (err.isEmpty()) {
        // many packages cannot be installed side-by-side and overwrite for example
        // the shortcuts of the old version in the start menu. We try to find
        // those packages where the old version can be uninstalled first and then
        // the new version installed. This is the reversed order for an update.
        // If this is possible and does not affect other packages, we do this first.
        for (int i = 0; i < newest.count(); i++) {
            QList<PackageVersion*> avoid;
            QList<InstallOperation*> ops2;
            QList<PackageVersion*> installedCopy = installed;

            QString err = newesti.at(i)->planUninstallation(installedCopy, ops2);
            if (err.isEmpty()) {
                err = newest.at(i)->planInstallation(installedCopy, ops2, avoid);
                if (err.isEmpty()) {
                    if (ops2.count() == 2) {
                        used[i] = true;
                        installed = installedCopy;
                        ops.append(ops2[0]);
                        ops.append(ops2[1]);
                        ops2.clear();
                    }
                }
            }

            qDeleteAll(ops2);
        }
    }

    if (err.isEmpty()) {
        for (int i = 0; i < newest.count(); i++) {
            if (!used[i]) {
                QList<PackageVersion*> avoid;
                err = newest.at(i)->planInstallation(installed, ops, avoid);
                if (!err.isEmpty())
                    break;
            }
        }
    }

    if (err.isEmpty()) {
        for (int i = 0; i < newesti.count(); i++) {
            if (!used[i]) {
                err = newesti.at(i)->planUninstallation(installed, ops);
                if (!err.isEmpty())
                    break;
            }
        }
    }

    if (err.isEmpty()) {
        InstallOperation::simplify(ops);
    }

    return err;
}

void Repository::detectWindows()
{
    OSVERSIONINFO osvi;
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&osvi);
    Version v;
    v.setVersion(osvi.dwMajorVersion, osvi.dwMinorVersion,
            osvi.dwBuildNumber);

    clearExternallyInstalled("com.microsoft.Windows");
    clearExternallyInstalled("com.microsoft.Windows32");
    clearExternallyInstalled("com.microsoft.Windows64");

    PackageVersion* pv = findOrCreatePackageVersion("com.microsoft.Windows", v);
    InstalledPackageVersion* ipv = new InstalledPackageVersion(
            pv->package_, pv->version, WPMUtils::getWindowsDir(), true);
    this->installedPackageVersions.append(ipv);
    if (WPMUtils::is64BitWindows()) {
        pv = findOrCreatePackageVersion("com.microsoft.Windows64", v);
        InstalledPackageVersion* ipv = new InstalledPackageVersion(
                pv->package_, pv->version, WPMUtils::getWindowsDir(), true);
        this->installedPackageVersions.append(ipv);
    } else {
        pv = findOrCreatePackageVersion("com.microsoft.Windows32", v);
        InstalledPackageVersion* ipv = new InstalledPackageVersion(
                pv->package_, pv->version, WPMUtils::getWindowsDir(), true);
        this->installedPackageVersions.append(ipv);
    }
}

void Repository::recognize(Job* job)
{
    job->setProgress(0);

    if (!job->isCancelled()) {
        job->setHint("Detecting Windows");
        detectWindows();
        job->setProgress(0.1);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting JRE");
        detectJRE(false);
        if (WPMUtils::is64BitWindows())
            detectJRE(true);
        job->setProgress(0.4);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting JDK");
        detectJDK(false);
        if (WPMUtils::is64BitWindows())
            detectJDK(true);
        job->setProgress(0.7);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting .NET");
        detectDotNet();
        job->setProgress(0.8);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting MSI packages");
        detectMSIProducts();
        job->setProgress(0.9);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting Windows Installer");
        detectMicrosoftInstaller();
        job->setProgress(0.95);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting Microsoft Core XML Services (MSXML)");
        detectMSXML();
        job->setProgress(0.97);
    }

    if (!job->isCancelled()) {
        job->setHint("Updating NPACKD_CL");
        updateNpackdCLEnvVar();
        job->setProgress(1);
    }

    job->complete();
}

void Repository::detectJRE(bool w64bit)
{
    clearExternallyInstalled(w64bit ? "com.oracle.JRE64" : "com.oracle.JRE");

    if (w64bit && !WPMUtils::is64BitWindows())
        return;

    WindowsRegistry jreWR;
    QString err = jreWR.open(HKEY_LOCAL_MACHINE,
            "Software\\JavaSoft\\Java Runtime Environment", !w64bit, KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = jreWR.list(&err);
        for (int i = 0; i < entries.count(); i++) {
            QString v_ = entries.at(i);
            v_ = v_.replace('_', '.');
            Version v;
            if (!v.setVersion(v_) || v.getNParts() <= 2)
                continue;

            WindowsRegistry wr;
            err = wr.open(jreWR, entries.at(i), KEY_READ);
            if (!err.isEmpty())
                continue;

            QString path = wr.get("JavaHome", &err);
            if (!err.isEmpty())
                continue;

            QDir d(path);
            if (!d.exists())
                continue;

            PackageVersion* pv = findOrCreatePackageVersion(
                    w64bit ? "com.oracle.JRE64" :
                    "com.oracle.JRE", v);
            InstalledPackageVersion* ipv =
                    this->findInstalledPackageVersion(pv);
            if (!ipv) {
                ipv = new InstalledPackageVersion(pv->package_, pv->version,
                        path, true);
                this->installedPackageVersions.append(ipv);
            }
        }
    }
}

void Repository::detectJDK(bool w64bit)
{
    QString p = w64bit ? "com.oracle.JDK64" : "com.oracle.JDK";

    clearExternallyInstalled(p);

    if (w64bit && !WPMUtils::is64BitWindows())
        return;

    WindowsRegistry wr;
    QString err = wr.open(HKEY_LOCAL_MACHINE,
            "Software\\JavaSoft\\Java Development Kit",
            !w64bit, KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = wr.list(&err);
        if (err.isEmpty()) {
            for (int i = 0; i < entries.count(); i++) {
                QString v_ = entries.at(i);
                WindowsRegistry r;
                err = r.open(wr, v_, KEY_READ);
                if (!err.isEmpty())
                    continue;

                v_.replace('_', '.');
                Version v;
                if (!v.setVersion(v_) || v.getNParts() <= 2)
                    continue;

                QString path = r.get("JavaHome", &err);
                if (!err.isEmpty())
                    continue;

                QDir d(path);
                if (!d.exists())
                    continue;

                PackageVersion* pv = findOrCreatePackageVersion(
                        p, v);
                InstalledPackageVersion* ipv =
                        this->findInstalledPackageVersion(pv);
                if (!ipv) {
                    ipv = new InstalledPackageVersion(pv->package_, pv->version,
                            path, true);
                    this->installedPackageVersions.append(ipv);
                }
            }
        }
    }
}

PackageVersion* Repository::findOrCreatePackageVersion(const QString &package,
        const Version &v)
{
    PackageVersion* pv = findPackageVersion(package, v);
    if (!pv) {
        Package* p = findPackage(package);
        if (!p) {
            p = new Package(package, package);
            this->addPackage(p);
        }

        pv = new PackageVersion(p);
        pv->version = v;
        pv->version.normalize();
        this->addPackageVersion(pv);
    }
    return pv;
}

void Repository::clearExternallyInstalled(QString package)
{
    Package* p = findPackage(package);
    if (p) {
        for (int i = 0; i < this->installedPackageVersions.count();) {
            InstalledPackageVersion* ipv = this->installedPackageVersions.at(i);
            if (ipv->package_ == p && ipv->external_) {
                this->installedPackageVersions.removeAt(i);
                delete ipv;
            } else {
                i++;
            }
        }
    }
}

void Repository::removeInstalledPackageVersion(PackageVersion* pv)
{
    InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
    if (ipv) {
        this->installedPackageVersions.removeOne(ipv);
        delete ipv;
    }
}

void Repository::detectOneDotNet(const WindowsRegistry& wr,
        const QString& keyName)
{
    QString packageName("com.microsoft.DotNetRedistributable");
    Version keyVersion;

    Version oneOne(1, 1);
    Version four(4, 0);
    Version two(2, 0);

    Version v;
    bool found = false;
    if (keyName.startsWith("v") && keyVersion.setVersion(
            keyName.right(keyName.length() - 1))) {
        if (keyVersion.compare(oneOne) < 0) {
            // not yet implemented
        } else if (keyVersion.compare(two) < 0) {
            v = keyVersion;
            found = true;
        } else if (keyVersion.compare(four) < 0) {
            QString err;
            QString value_ = wr.get("Version", &err);
            if (err.isEmpty() && v.setVersion(value_)) {
                found = true;
            }
        } else {
            WindowsRegistry r;
            QString err = r.open(wr, "Full", KEY_READ);
            if (err.isEmpty()) {
                QString value_ = r.get("Version", &err);
                if (err.isEmpty() && v.setVersion(value_)) {
                    found = true;
                }
            }
        }
    }

    if (found) {
        PackageVersion* pv = findOrCreatePackageVersion(packageName, v);
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        if (!ipv) {
            ipv = new InstalledPackageVersion(pv->package_, pv->version,
                    WPMUtils::getWindowsDir(), true);
            this->installedPackageVersions.append(ipv);
            // qDebug() << pv->version.getVersionString() << " kkk ";
        }
    }
}

void Repository::detectMSIProducts()
{
    QStringList all = WPMUtils::findInstalledMSIProducts();
    // qDebug() << all.at(0);

    for (int i = 0; i < this->getPackageVersionCount(); i++) {
        PackageVersion* pv = this->getPackageVersion(i);
        if (pv->msiGUID.length() == 38) {
            // qDebug() << pv->msiGUID;
            if (all.contains(pv->msiGUID)) {
                // qDebug() << pv->toString();
                InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
                if (!ipv || ipv->external_) {
                    QString err;
                    QString p = WPMUtils::getMSIProductLocation(
                            pv->msiGUID, &err);
                    if (p.isEmpty() || !err.isEmpty())
                        p = WPMUtils::getWindowsDir();

                    if (!ipv) {
                        ipv = new InstalledPackageVersion(pv->package_,
                                pv->version, p, true);
                        this->installedPackageVersions.append(ipv);
                    } else {
                        ipv->ipath = p;
                        ipv->external_ = true;
                    }
                }
            } else {
                removeInstalledPackageVersion(pv);
            }
        }
    }
}

void Repository::detectDotNet()
{
    // http://stackoverflow.com/questions/199080/how-to-detect-what-net-framework-versions-and-service-packs-are-installed

    clearExternallyInstalled("com.microsoft.DotNetRedistributable");

    WindowsRegistry wr;
    QString err = wr.open(HKEY_LOCAL_MACHINE,
            "Software\\Microsoft\\NET Framework Setup\\NDP", false, KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = wr.list(&err);
        if (err.isEmpty()) {
            for (int i = 0; i < entries.count(); i++) {
                QString v_ = entries.at(i);
                Version v;
                if (v_.startsWith("v") && v.setVersion(
                        v_.right(v_.length() - 1))) {
                    WindowsRegistry r;
                    err = r.open(wr, v_, KEY_READ);
                    if (err.isEmpty())
                        detectOneDotNet(r, v_);
                }
            }
        }
    }
}

void Repository::detectMicrosoftInstaller()
{
    clearExternallyInstalled("com.microsoft.WindowsInstaller");

    Version v = WPMUtils::getDLLVersion("MSI.dll");
    Version nullNull(0, 0);
    if (v.compare(nullNull) > 0) {
        PackageVersion* pv = findOrCreatePackageVersion(
                "com.microsoft.WindowsInstaller", v);
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        if (!ipv) {
            ipv = new InstalledPackageVersion(pv->package_, pv->version,
                    WPMUtils::getWindowsDir(), true);
            this->installedPackageVersions.append(ipv);
        }
    }
}

void Repository::detectMSXML()
{
    clearExternallyInstalled("com.microsoft.MSXML");

    Version v = WPMUtils::getDLLVersion("msxml.dll");
    Version nullNull(0, 0);
    if (v.compare(nullNull) > 0) {
        PackageVersion* pv = findOrCreatePackageVersion(
                "com.microsoft.MSXML", v);
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        if (!ipv) {
            ipv = new InstalledPackageVersion(pv->package_, pv->version,
                    WPMUtils::getWindowsDir(), true);
            this->installedPackageVersions.append(ipv);
        }
    }
    v = WPMUtils::getDLLVersion("msxml2.dll");
    if (v.compare(nullNull) > 0) {
        PackageVersion* pv = findOrCreatePackageVersion(
                "com.microsoft.MSXML", v);
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        if (!ipv) {
            ipv = new InstalledPackageVersion(pv->package_, pv->version,
                    WPMUtils::getWindowsDir(), true);
            this->installedPackageVersions.append(ipv);
        }
    }
    v = WPMUtils::getDLLVersion("msxml3.dll");
    if (v.compare(nullNull) > 0) {
        v.prepend(3);
        PackageVersion* pv = findOrCreatePackageVersion(
                "com.microsoft.MSXML", v);
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        if (!ipv) {
            ipv = new InstalledPackageVersion(pv->package_, pv->version,
                    WPMUtils::getWindowsDir(), true);
            this->installedPackageVersions.append(ipv);
        }
    }
    v = WPMUtils::getDLLVersion("msxml4.dll");
    if (v.compare(nullNull) > 0) {
        PackageVersion* pv = findOrCreatePackageVersion(
                "com.microsoft.MSXML", v);
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        if (!ipv) {
            ipv = new InstalledPackageVersion(pv->package_, pv->version,
                    WPMUtils::getWindowsDir(), true);
            this->installedPackageVersions.append(ipv);
        }
    }
    v = WPMUtils::getDLLVersion("msxml5.dll");
    if (v.compare(nullNull) > 0) {
        PackageVersion* pv = findOrCreatePackageVersion(
                "com.microsoft.MSXML", v);
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        if (!ipv) {
            ipv = new InstalledPackageVersion(pv->package_, pv->version,
                    WPMUtils::getWindowsDir(), true);
            this->installedPackageVersions.append(ipv);
        }
    }
    v = WPMUtils::getDLLVersion("msxml6.dll");
    if (v.compare(nullNull) > 0) {
        PackageVersion* pv = findOrCreatePackageVersion(
                "com.microsoft.MSXML", v);
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        if (!ipv) {
            ipv = new InstalledPackageVersion(pv->package_, pv->version,
                    WPMUtils::getWindowsDir(), true);
            this->installedPackageVersions.append(ipv);
        }
    }
}

PackageVersion* Repository::findPackageVersion(const QString& package,
        const Version& version) const
{
    PackageVersion* r = 0;

    QList<PackageVersion*> list = this->getPackageVersions(package);
    for (int i = 0; i < list.count(); i++) {
        PackageVersion* p = list.at(i);
        if (p->version.compare(version) == 0) {
            r = p;
            break;
        }
    }
    return r;
}

QString Repository::writeTo(const QString& filename) const
{
    QString r;

    QDomDocument doc;
    QDomElement root = doc.createElement("root");
    doc.appendChild(root);

    XMLUtils::addTextTag(root, "spec-version", "3");

    for (int i = 0; i < getPackageCount(); i++) {
        Package* p = packages.at(i);
        QDomElement package = doc.createElement("package");
        package.setAttribute("name", p->name);
        XMLUtils::addTextTag(package, "title", p->title);
        if (!p->description.isEmpty())
            XMLUtils::addTextTag(package, "description", p->description);
        root.appendChild(package);
    }

    for (int i = 0; i < getPackageVersionCount(); i++) {
        PackageVersion* pv = getPackageVersion(i);
        QDomElement version = doc.createElement("version");
        version.setAttribute("name", pv->version.getVersionString());
        version.setAttribute("package", pv->getPackage()->name);
        if (pv->download.isValid())
            XMLUtils::addTextTag(version, "url", pv->download.toString());
        root.appendChild(version);
    }

    QFile file(filename);
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream s(&file);
        doc.save(s, 4);
    } else {
        r = QString("Cannot open %1 for writing").arg(filename);
    }

    return "";
}

void Repository::process(Job *job, const QList<InstallOperation *> &install)
{
    for (int j = 0; j < install.size(); j++) {
        InstallOperation* op = install.at(j);
        PackageVersion* pv = op->packageVersion;
        pv->lock();
    }

    int n = install.count();

    for (int i = 0; i < install.count(); i++) {
        InstallOperation* op = install.at(i);
        PackageVersion* pv = op->packageVersion;
        if (op->install)
            job->setHint(QString("Installing %1").arg(
                    pv->toString()));
        else
            job->setHint(QString("Uninstalling %1").arg(
                    pv->toString()));
        Job* sub = job->newSubJob(1.0 / n);
        if (op->install)
            pv->install(sub, pv->getPreferredInstallationDirectory());
        else
            pv->uninstall(sub);
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(sub->getErrorMessage());
        delete sub;

        if (!job->getErrorMessage().isEmpty())
            break;
    }

    for (int j = 0; j < install.size(); j++) {
        InstallOperation* op = install.at(j);
        PackageVersion* pv = op->packageVersion;
        pv->unlock();
    }

    job->complete();
}

void Repository::scanPre1_15Dir(bool exact)
{
    QDir aDir(WPMUtils::getInstallationDirectory());
    if (!aDir.exists())
        return;

    WindowsRegistry machineWR(HKEY_LOCAL_MACHINE, false);
    QString err;
    WindowsRegistry packagesWR = machineWR.createSubKey(
            "SOFTWARE\\Npackd\\Npackd\\Packages", &err);
    if (!err.isEmpty())
        return;

    QFileInfoList entries = aDir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::Dirs);
    int count = entries.size();
    QString dirPath = aDir.absolutePath();
    dirPath.replace('/', '\\');
    for (int idx = 0; idx < count; idx++) {
        QFileInfo entryInfo = entries[idx];
        QString name = entryInfo.fileName();
        int pos = name.lastIndexOf("-");
        if (pos > 0) {
            QString packageName = name.left(pos);
            QString versionName = name.right(name.length() - pos - 1);

            if (Package::isValidName(packageName)) {
                Version version;
                if (version.setVersion(versionName)) {
                    if (!exact || this->findPackage(packageName)) {
                        // using getVersionString() here to fix a bug in earlier
                        // versions where version numbers were not normalized
                        WindowsRegistry wr = packagesWR.createSubKey(
                                packageName + "-" + version.getVersionString(),
                                &err);
                        if (err.isEmpty()) {
                            wr.set("Path", dirPath + "\\" +
                                    name);
                            wr.setDWORD("External", 0);
                        }
                    }
                }
            }
        }
    }
}

QString Repository::computeNpackdCLEnvVar()
{
    QString v;
    PackageVersion* pv = findNewestInstalledPackageVersion(
            "com.googlecode.windows-package-manager.NpackdCL");
    if (pv) {
        InstalledPackageVersion* ipv = findInstalledPackageVersion(pv);
        v = ipv->ipath;
    }

    return v;
}

void Repository::updateNpackdCLEnvVar()
{
    QString v = computeNpackdCLEnvVar();

    // ignore the error for the case NPACKD_CL does not yet exist
    QString err;
    QString cur = WPMUtils::getSystemEnvVar("NPACKD_CL", &err);

    if (v != cur) {
        if (WPMUtils::setSystemEnvVar("NPACKD_CL", v).isEmpty())
            WPMUtils::fireEnvChanged();
    }
}

void Repository::detectPre_1_15_Packages()
{
    QString regPath = "SOFTWARE\\Npackd\\Npackd";
    WindowsRegistry machineWR(HKEY_LOCAL_MACHINE, false);
    QString err;
    WindowsRegistry npackdWR = machineWR.createSubKey(regPath, &err);
    if (err.isEmpty()) {
        DWORD b = npackdWR.getDWORD("Pre1_15DirScanned", &err);
        if (!err.isEmpty() || b != 1) {
            // store the references to packages in the old format (< 1.15)
            // in the registry
            scanPre1_15Dir(false);
            npackdWR.setDWORD("Pre1_15DirScanned", 1);
        }
    }
}

int Repository::getPackageCount() const {
    return this->packages.count();
}

int Repository::getPackageVersionCount() const {
    return this->packageVersions.count();
}

PackageVersion* Repository::getPackageVersion(int i) const {
    return this->packageVersions.at(i);
}

void Repository::readRegistryDatabase()
{
    qDeleteAll(this->installedPackageVersions);
    this->installedPackageVersions.clear();

    WindowsRegistry machineWR(HKEY_LOCAL_MACHINE, false, KEY_READ);

    QString err;
    WindowsRegistry packagesWR;
    err = packagesWR.open(machineWR,
            "SOFTWARE\\Npackd\\Npackd\\Packages", KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = packagesWR.list(&err);
        for (int i = 0; i < entries.count(); ++i) {
            QString name = entries.at(i);
            int pos = name.lastIndexOf("-");
            if (pos > 0) {
                QString packageName = name.left(pos);
                if (Package::isValidName(packageName)) {
                    QString versionName = name.right(name.length() - pos - 1);
                    Version version;
                    if (version.setVersion(versionName)) {
                        PackageVersion* pv = findOrCreatePackageVersion(
                                packageName, version);
                        loadInstallationInfoFromRegistry(pv->package_,
                                pv->version);
                    }
                }
            }
        }
    }
}

void Repository::loadInstallationInfoFromRegistry(Package* package,
        const Version& version)
{
    WindowsRegistry entryWR;
    QString err = entryWR.open(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Npackd\\Npackd\\Packages\\" +
            package->name + "-" + version.getVersionString(),
            false, KEY_READ);
    if (!err.isEmpty())
        return;

    QString p = entryWR.get("Path", &err).trimmed();
    if (!err.isEmpty())
        return;

    DWORD external = entryWR.getDWORD("External", &err);
    if (!err.isEmpty())
        external = 1;

    QString ipath;
    if (p.isEmpty())
        ipath = "";
    else {
        QDir d(p);
        if (d.exists()) {
            ipath = p;
        } else {
            ipath = "";
        }
    }

    if (!ipath.isEmpty()) {
        InstalledPackageVersion* ipv = new InstalledPackageVersion(package,
                version, ipath, external != 0);
        this->installedPackageVersions.append(ipv);
    }

    // TODO: emitStatusChanged();
}

InstalledPackageVersion* Repository::findInstalledPackageVersion(
        const PackageVersion* pv)
{
    InstalledPackageVersion* r = 0;
    for (int i = 0; i < this->installedPackageVersions.count(); i++) {
        InstalledPackageVersion* ipv = this->installedPackageVersions.at(i);
        if (ipv->package_ == pv->package_ && ipv->version == pv->version) {
            r = ipv;
            break;
        }
    }
    return r;
}

void Repository::scan(const QString& path, Job* job, int level,
        QStringList& ignore)
{
    if (ignore.contains(path))
        return;

    QDir aDir(path);

    QMap<QString, QString> path2sha1;

    for (int i = 0; i < this->getPackageVersionCount(); i++) {
        if (job && job->isCancelled())
            break;

        PackageVersion* pv = this->getPackageVersion(i);
        InstalledPackageVersion* ipv = this->findInstalledPackageVersion(pv);
        if (!ipv && pv->detectFiles.count() > 0) {
            boolean ok = true;
            for (int j = 0; j < pv->detectFiles.count(); j++) {
                bool fileOK = false;
                DetectFile* df = pv->detectFiles.at(j);
                if (aDir.exists(df->path)) {
                    QString fullPath = path + "\\" + df->path;
                    QFileInfo f(fullPath);
                    if (f.isFile() && f.isReadable()) {
                        QString sha1 = path2sha1[df->path];
                        if (sha1.isEmpty()) {
                            sha1 = WPMUtils::sha1(fullPath);
                            path2sha1[df->path] = sha1;
                        }
                        if (df->sha1 == sha1) {
                            fileOK = true;
                        }
                    }
                }
                if (!fileOK) {
                    ok = false;
                    break;
                }
            }

            if (ok) {
                ipv = new InstalledPackageVersion(pv->package_, pv->version,
                        path, true);
                this->installedPackageVersions.append(ipv);
                return;
            }
        }
    }

    if (job && !job->isCancelled()) {
        QFileInfoList entries = aDir.entryInfoList(
                QDir::NoDotAndDotDot | QDir::Dirs);
        int count = entries.size();
        for (int idx = 0; idx < count; idx++) {
            if (job && job->isCancelled())
                break;

            QFileInfo entryInfo = entries[idx];
            QString name = entryInfo.fileName();

            if (job) {
                job->setHint(QString("%1").arg(name));
                if (job->isCancelled())
                    break;
            }

            Job* djob;
            if (level < 2)
                djob = job->newSubJob(1.0 / count);
            else
                djob = 0;
            scan(path + "\\" + name.toLower(), djob, level + 1, ignore);
            delete djob;

            if (job) {
                job->setProgress(((double) idx) / count);
            }
        }
    }

    if (job)
        job->complete();
}

void Repository::scanHardDrive(Job* job)
{
    QStringList ignore;
    ignore.append(WPMUtils::normalizePath(WPMUtils::getWindowsDir()));

    QFileInfoList fil = QDir::drives();
    for (int i = 0; i < fil.count(); i++) {
        if (job->isCancelled())
            break;

        QFileInfo fi = fil.at(i);

        job->setHint(QString("Scanning %1").arg(fi.absolutePath()));
        Job* djob = job->newSubJob(1.0 / fil.count());
        QString path = WPMUtils::normalizePath(fi.absolutePath());
        UINT t = GetDriveType((WCHAR*) path.utf16());
        if (t == DRIVE_FIXED)
            scan(path, djob, 0, ignore);
        delete djob;
    }

    job->complete();
}

void Repository::reload(Job *job)
{
    /* debugging
    QList<PackageVersion*> msw = this->getPackageVersions(
            "com.microsoft.Windows");
    qDebug() << "Repository::recognize " << msw.count();
    for (int i = 0; i < msw.count(); i++) {
        qDebug() << msw.at(i)->toString() << " " << msw.at(i)->getStatus();
    }
    */

    job->setHint("Loading repositories");

    this->clearPackages();
    this->clearPackageVersions();

    QList<QUrl*> urls = getRepositoryURLs();
    QString key;
    if (urls.count() > 0) {
        for (int i = 0; i < urls.count(); i++) {
            job->setHint(QString("Repository %1 of %2").arg(i + 1).
                         arg(urls.count()));
            Job* s = job->newSubJob(0.5 / urls.count());
            QString sha1;
            loadOne(urls.at(i), s, &sha1);
            key += sha1;
            if (!s->getErrorMessage().isEmpty()) {
                job->setErrorMessage(QString(
                        "Error loading the repository %1: %2").arg(
                        urls.at(i)->toString()).arg(s->getErrorMessage()));
                delete s;
                break;
            }
            delete s;

            if (job->isCancelled())
                break;
        }
    } else {
        job->setErrorMessage("No repositories defined");
        job->setProgress(0.5);
    }

    key.append("2"); // serialization version

    qDeleteAll(urls);

    // TODO: can we apply SHA1 to itself?
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(key.toAscii());
    key = hash.result().toHex().toLower();

    bool indexed = false;
    WindowsRegistry wr;
    QString err = wr.open(HKEY_LOCAL_MACHINE,
            "Software\\Npackd\\Npackd\\Index", false,
            KEY_READ);
    if (err.isEmpty()) {
        QString storedKey = wr.get("SHA1", &err);
        if (err.isEmpty() && key == storedKey) {
            indexed = true;
        }
    }

    // qDebug() << "Repository::load.3";

    job->complete();

    addWellKnownPackages();

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        Job* d = job->newSubJob(0.1);
        job->setHint("Refreshing installation statuses");
        refresh(d);
        delete d;
    }

    QString fn;
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Creating index directory");

        // Open the database for update, creating a new database if necessary.
        fn = WPMUtils::getShellDir(CSIDL_LOCAL_APPDATA) +
                "\\Npackd\\Npackd";
        QDir dir(fn);
        if (!dir.exists(fn)) {
            if (!dir.mkpath(fn)) {
                job->setErrorMessage(
                        QString("Cannot create the directory %1").arg(fn));
            }
        }

        job->setProgress(0.65);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        delete this->enquire;
        delete this->queryParser;
        delete db;

        QString indexDir = fn + "\\Index";
        QDir d(indexDir);

        if (!d.exists())
            indexed = false;

        // TODO: index cannot be reopened
        if (!indexed) {
            // TODO: catch Xapian exceptions here
            db = new Xapian::WritableDatabase(
                    indexDir.toUtf8().constData(),
                    Xapian::DB_CREATE_OR_OVERWRITE);

            Job* sub = job->newSubJob(0.35);
            this->index(sub);
            if (!sub->getErrorMessage().isEmpty())
                job->setErrorMessage(sub->getErrorMessage());
            delete sub;

            WindowsRegistry wr;
            QString err = wr.open(HKEY_LOCAL_MACHINE,
                    "Software", false, KEY_ALL_ACCESS);
            if (err.isEmpty()) {
                WindowsRegistry indexReg = wr.createSubKey(
                        "Npackd\\Npackd\\Index", &err, KEY_ALL_ACCESS);
                if (err.isEmpty()) {
                    indexReg.set("SHA1", key);
                }
            }
        } else {
            // TODO: catch Xapian exceptions here
            db = new Xapian::WritableDatabase(
                    indexDir.toUtf8().constData(),
                    Xapian::DB_CREATE_OR_OPEN);
            job->setProgress(1);
        }

        this->enquire = new Xapian::Enquire(*this->db);
        this->queryParser = new Xapian::QueryParser();
        this->queryParser->set_database(*this->db);
        queryParser->set_stemmer(stemmer);
        queryParser->set_default_op(Xapian::Query::OP_AND);
    }

    job->complete();

    /* debugging
    msw = this->getPackageVersions(
            "com.microsoft.Windows");
    qDebug() << "Repository::recognize 2 " << msw.count();
    for (int i = 0; i < msw.count(); i++) {
        qDebug() << msw.at(i)->toString() << " " << msw.at(i)->getStatus();
    }
    */
}

void Repository::refresh(Job *job)
{
    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting packages installed by Npackd 1.14 or earlier");
        this->detectPre_1_15_Packages();
        job->setProgress(0.4);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Reading registry package database");
        this->readRegistryDatabase();
        job->setProgress(0.5);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting software");
        Job* d = job->newSubJob(0.2);
        this->recognize(d);
        delete d;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting packages installed by Npackd 1.14 or earlier (2)");
        scanPre1_15Dir(true);
        job->setProgress(1);
    }

    job->complete();
}

void Repository::loadOne(QUrl* url, Job* job, QString* sha1) {
    job->setHint("Downloading");

    QTemporaryFile* f = 0;
    if (job->getErrorMessage().isEmpty() && !job->isCancelled()) {
        Job* djob = job->newSubJob(0.90);
        f = Downloader::download(djob, *url, sha1, true);
        if (!djob->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Download failed: %2").
                    arg(djob->getErrorMessage()));
        delete djob;
    }

    QDomDocument doc;
    if (job->getErrorMessage().isEmpty() && !job->isCancelled()) {
        job->setHint("Parsing the content");
        // qDebug() << "Repository::loadOne.2";
        int errorLine;
        int errorColumn;
        QString errMsg;
        if (!doc.setContent(f, &errMsg, &errorLine, &errorColumn))
            job->setErrorMessage(QString("XML parsing failed: %1").
                                 arg(errMsg));
        else
            job->setProgress(0.91);
    }

    if (job->getErrorMessage().isEmpty() && !job->isCancelled()) {
        Job* djob = job->newSubJob(0.09);
        loadOne(&doc, djob);
        if (!djob->getErrorMessage().isEmpty())
            job->setErrorMessage(QString("Error loading XML: %2").
                    arg(djob->getErrorMessage()));
        delete djob;
    }

    delete f;

    job->complete();
}

void Repository::addPackage(Package* p) {
    this->packages.append(p);
    this->nameToPackage.insert(p->name, p);
}

bool packageVersionLessThan(const PackageVersion* pv1, const PackageVersion* pv2)
{
    return pv1->version.compare(pv2->version) < 0;
}

QList<PackageVersion*> Repository::getPackageVersions(QString package) const {
    QList<PackageVersion*> list = this->nameToPackageVersion.values(package);
    qSort(list.begin(), list.end(), packageVersionLessThan);
    return list;
}

void Repository::addPackageVersion(PackageVersion* pv) {
    this->packageVersions.append(pv);
    this->nameToPackageVersion.insert(pv->getPackage()->name, pv);
}

void Repository::clearPackages() {
    qDeleteAll(this->packages);
    this->packages.clear();
    this->nameToPackage.clear();
}

void Repository::clearPackageVersions() {
    qDeleteAll(this->packageVersions);
    this->packageVersions.clear();
    this->nameToPackageVersion.clear();
}

void Repository::loadOne(QDomDocument* doc, Job* job)
{
    QDomElement root;
    if (job->getErrorMessage().isEmpty() && !job->isCancelled()) {
        root = doc->documentElement();
        QDomNodeList nl = root.elementsByTagName("spec-version");
        if (nl.count() != 0) {
            QString specVersion = nl.at(0).firstChild().nodeValue();
            Version specVersion_;
            if (!specVersion_.setVersion(specVersion)) {
                job->setErrorMessage(QString(
                        "Invalid repository specification version: %1").
                        arg(specVersion));
            } else {
                if (specVersion_.compare(Version(4, 0)) >= 0)
                    job->setErrorMessage(QString(
                            "Incompatible repository specification version: %1. \n"
                            "Plese download a newer version of Npackd from http://code.google.com/p/windows-package-manager/").
                            arg(specVersion));
                else
                    job->setProgress(0.01);
            }
        } else {
            job->setProgress(0.01);
        }
    }

    if (job->getErrorMessage().isEmpty() && !job->isCancelled()) {
        for (QDomNode n = root.firstChild(); !n.isNull();
                n = n.nextSibling()) {
            if (n.isElement()) {
                QDomElement e = n.toElement();
                if (e.nodeName() == "license") {
                    License* p = createLicense(&e);
                    if (this->findLicense(p->name))
                        delete p;
                    else
                        this->licenses.append(p);
                }
            }
        }
        for (QDomNode n = root.firstChild(); !n.isNull();
                n = n.nextSibling()) {
            if (n.isElement()) {
                QDomElement e = n.toElement();
                if (e.nodeName() == "package") {
                    QString err;
                    Package* p = createPackage(&e, &err);
                    if (p) {
                        if (this->findPackage(p->name))
                            delete p;
                        else
                           this->addPackage(p);
                    } else {
                        job->setErrorMessage(err);
                        break;
                    }
                }
            }
        }
        for (QDomNode n = root.firstChild(); !n.isNull();
                n = n.nextSibling()) {
            if (n.isElement()) {
                QDomElement e = n.toElement();
                if (e.nodeName() == "version") {
                    QString err;
                    PackageVersion* pv = PackageVersion::createPackageVersion(
                            &e, &err);
                    if (pv) {
                        if (this->findPackageVersion(pv->getPackage()->name,
                                pv->version))
                            delete pv;
                        else {
                            this->addPackageVersion(pv);
                        }
                    } else {
                        job->setErrorMessage(err);
                        break;
                    }
                }
            }
        }
        job->setProgress(1);
    }

    job->complete();
}

void Repository::fireStatusChanged(PackageVersion *pv)
{
    emit statusChanged(pv);
}

PackageVersion* Repository::findLockedPackageVersion() const
{
    PackageVersion* r = 0;
    for (int i = 0; i < getPackageVersionCount(); i++) {
        PackageVersion* pv = getPackageVersion(i);
        if (pv->isLocked()) {
            r = pv;
            break;
        }
    }
    return r;
}

QList<QUrl*> Repository::getRepositoryURLs()
{
    QList<QUrl*> r;

    WindowsRegistry wr;
    if (wr.open(HKEY_LOCAL_MACHINE,
            "Software\\Npackd\\Npackd\\Reps", false, KEY_READ).isEmpty()) {
        QString err;
        int count = wr.getDWORD("Count", &err);
        if (err.isEmpty()) {
            for (int i = 0; i < count; i++) {
                WindowsRegistry wr2;
                if (wr2.open(wr, QString("%1").arg(i)).isEmpty()) {
                    QString url = wr2.get("URL", &err);
                    if (err.isEmpty()) {
                        r.append(new QUrl(url));
                    }
                }
            }
        }
    } else {
        if (r.size() == 0) {
            QSettings s1("Npackd", "Npackd");
            int size = s1.beginReadArray("repositories");
            for (int i = 0; i < size; ++i) {
                s1.setArrayIndex(i);
                QString v = s1.value("repository").toString();
                r.append(new QUrl(v));
            }
            s1.endArray();
        }

        if (r.size() == 0) {
            QSettings s("WPM", "Windows Package Manager");

            int size = s.beginReadArray("repositories");
            for (int i = 0; i < size; ++i) {
                s.setArrayIndex(i);
                QString v = s.value("repository").toString();
                r.append(new QUrl(v));
            }
            s.endArray();

            if (size == 0) {
                QString v = s.value("repository", "").toString();
                if (v != "") {
                    r.append(new QUrl(v));
                }
            }
        }

        setRepositoryURLs(r);
    }
    
    return r;
}

void Repository::setRepositoryURLs(QList<QUrl*>& urls)
{
    WindowsRegistry wr;
    if (wr.open(HKEY_LOCAL_MACHINE, "Software", false).isEmpty()) {
        QString err;
        WindowsRegistry wr2 = wr.createSubKey("Npackd\\Npackd\\Reps", &err);
        if (err.isEmpty()) {
            err = wr2.setDWORD("Count", urls.count());
            if (err.isEmpty()) {
                for (int i = 0; i < urls.count(); i++) {
                    WindowsRegistry wr3 = wr2.createSubKey(
                            QString("%1").arg(i), &err);
                    if (err.isEmpty()) {
                        wr3.set("URL", urls.at(i)->toString());
                    }
                }
            }
        }
    }
}

Repository* Repository::getDefault()
{
    return &def;
}
