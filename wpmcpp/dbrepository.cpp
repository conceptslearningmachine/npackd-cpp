#include "dbrepository.h"

#include <shlobj.h>

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QDir>
#include <QVariant>
#include <QDomDocument>
#include <QDomElement>
#include <QTextStream>
#include <QByteArray>
#include <QDebug>

#include "package.h"
#include "repository.h"
#include "packageversion.h"
#include "wpmutils.h"
#include "installedpackages.h"
#include "hrtimer.h"

static bool packageVersionLessThan3(const PackageVersion* a,
        const PackageVersion* b)
{
    int r = a->package.compare(b->package);
    if (r == 0) {
        r = a->version.compare(b->version);
    }

    return r > 0;
}

class QMySqlQuery: public QSqlQuery {
public:
    bool exec(const QString& query);
    bool exec();
};

bool QMySqlQuery::exec(const QString &query)
{
    //DWORD start = GetTickCount();
    bool r = QSqlQuery::exec(query);
    // qDebug() << query << (GetTickCount() - start);
    return r;
}

bool QMySqlQuery::exec()
{
    //DWORD start = GetTickCount();
    bool r = QSqlQuery::exec();
    // qDebug() << this->lastQuery() << (GetTickCount() - start);
    return r;
}

DBRepository DBRepository::def;

DBRepository::DBRepository()
{
}

DBRepository::~DBRepository()
{
}

DBRepository* DBRepository::getDefault()
{
    return &def;
}

QString DBRepository::exec(const QString& sql)
{
    QMySqlQuery q;
    q.exec(sql);
    return toString(q.lastError());
}

QString DBRepository::saveLicense(License* p, bool replace)
{
    QMySqlQuery q;

    QString sql = "INSERT OR ";
    if (replace)
        sql += "REPLACE";
    else
        sql += "IGNORE";
    sql += " INTO LICENSE "
            "(NAME, TITLE, DESCRIPTION, URL)"
            "VALUES(:NAME, :TITLE, :DESCRIPTION, :URL)";
    q.prepare(sql);
    q.bindValue(":NAME", p->name);
    q.bindValue(":TITLE", p->title);
    q.bindValue(":DESCRIPTION", p->description);
    q.bindValue(":URL", p->url);
    q.exec();
    return toString(q.lastError());
}

bool DBRepository::tableExists(QSqlDatabase* db,
        const QString& table, QString* err)
{
    *err = "";
    QMySqlQuery q;
    q.prepare("SELECT name FROM sqlite_master WHERE "
            "type='table' AND name=:NAME");
    q.bindValue(":NAME", table);
    q.exec();
    *err = toString(q.lastError());

    bool e = false;
    if (err->isEmpty()) {
        e = q.next();
    }
    return e;
}

Package *DBRepository::findPackage_(const QString &name)
{
    Package* r = 0;

    QMySqlQuery q;
    q.prepare("SELECT NAME, TITLE, URL, ICON, "
            "DESCRIPTION, LICENSE, CATEGORY0, CATEGORY1, CATEGORY2, "
            "CATEGORY3, CATEGORY4 "
            "FROM PACKAGE WHERE NAME = :NAME");
    q.bindValue(":NAME", name);
    q.exec();
    if (q.next()) {
        Package* p = new Package(q.value(0).toString(), q.value(1).toString());
        p->url = q.value(2).toString();
        p->icon = q.value(3).toString();
        p->description = q.value(4).toString();
        p->license = q.value(5).toString();
        int cat0 = q.value(6).toInt();
        int cat1 = q.value(7).toInt();
        int cat2 = q.value(8).toInt();
        int cat3 = q.value(9).toInt();
        int cat4 = q.value(10).toInt();

        int cat = cat4;
        if (cat <= 0)
            cat = cat3;
        if (cat <= 0)
            cat = cat2;
        if (cat <= 0)
            cat = cat1;
        if (cat <= 0)
            cat = cat0;

        if (cat > 0) {
            QString category = findCategory(cat);
            if (!category.isEmpty())
                p->categories.append(category);
        }

        r = p;
    }

    return r;
}

QString DBRepository::findCategory(int cat) const
{
    return categories.value(cat);
}

PackageVersion* DBRepository::findPackageVersion_(
        const QString& package, const Version& version, QString* err)
{
    *err = "";

    QString version_ = version.getVersionString();
    PackageVersion* r = 0;

    QMySqlQuery q;
    q.prepare("SELECT NAME, "
            "PACKAGE, CONTENT, MSIGUID FROM PACKAGE_VERSION "
            "WHERE NAME = :NAME AND PACKAGE = :PACKAGE");
    q.bindValue(":NAME", version_);
    q.bindValue(":PACKAGE", package);
    q.exec();
    if (q.next()) {
        QDomDocument doc;
        int errorLine, errorColumn;
        if (!doc.setContent(q.value(2).toByteArray(),
                err, &errorLine, &errorColumn))
            *err = QString(
                    QObject::tr("XML parsing failed at line %1, column %2: %3")).
                    arg(errorLine).arg(errorColumn).arg(*err);

        if (err->isEmpty()) {
            QDomElement root = doc.documentElement();
            PackageVersion* p = PackageVersion::parse(&root, err);

            if (err->isEmpty()) {
                r = p;
            }
        }
    }

    return r;
}

QList<PackageVersion*> DBRepository::getPackageVersions_(const QString& package,
        QString *err) const
{
    *err = "";

    QList<PackageVersion*> r;

    QMySqlQuery q;
    q.prepare("SELECT NAME, "
            "PACKAGE, CONTENT, MSIGUID FROM PACKAGE_VERSION "
            "WHERE PACKAGE = :PACKAGE");
    q.bindValue(":PACKAGE", package);
    if (!q.exec()) {
        *err = toString(q.lastError());
    }

    while (err->isEmpty() && q.next()) {
        QDomDocument doc;
        int errorLine, errorColumn;
        if (!doc.setContent(q.value(2).toByteArray(),
                err, &errorLine, &errorColumn)) {
            *err = QString(
                    QObject::tr("XML parsing failed at line %1, column %2: %3")).
                    arg(errorLine).arg(errorColumn).arg(*err);
        }

        QDomElement root = doc.documentElement();

        if (err->isEmpty()) {
            PackageVersion* pv = PackageVersion::parse(&root, err);
            if (err->isEmpty())
                r.append(pv);
        }
    }

    // qDebug() << vs.count();

    qSort(r.begin(), r.end(), packageVersionLessThan3);

    return r;
}

QList<PackageVersion *> DBRepository::getPackageVersionsWithDetectFiles(
        QString *err) const
{
    *err = "";

    QList<PackageVersion*> r;

    QMySqlQuery q;
    q.prepare("SELECT CONTENT FROM PACKAGE_VERSION "
            "WHERE DETECT_FILE_COUNT > 0");
    if (!q.exec()) {
        *err = toString(q.lastError());
    }

    while (err->isEmpty() && q.next()) {
        QDomDocument doc;
        int errorLine, errorColumn;
        if (!doc.setContent(q.value(0).toByteArray(),
                err, &errorLine, &errorColumn)) {
            *err = QString(
                    QObject::tr("XML parsing failed at line %1, column %2: %3")).
                    arg(errorLine).arg(errorColumn).arg(*err);
        }

        QDomElement root = doc.documentElement();

        if (err->isEmpty()) {
            PackageVersion* pv = PackageVersion::parse(&root, err);
            if (err->isEmpty())
                r.append(pv);
        }
    }

    // qDebug() << vs.count();

    qSort(r.begin(), r.end(), packageVersionLessThan3);

    return r;
}

License *DBRepository::findLicense_(const QString& name, QString *err)
{
    *err = "";

    License* r = 0;
    License* cached = this->licenses.object(name);
    if (!cached) {
        QMySqlQuery q;
        q.prepare("SELECT NAME, TITLE, DESCRIPTION, URL "
                "FROM LICENSE "
                "WHERE NAME = :NAME");
        q.bindValue(":NAME", name);
        if (!q.exec())
            *err = toString(q.lastError());

        if (err->isEmpty()) {
            if (q.next()) {
                cached = new License(name, q.value(1).toString());
                cached->description = q.value(2).toString();
                cached->url = q.value(3).toString();
                r = cached->clone();
                this->licenses.insert(name, cached);
            }
        }
    } else {
        r = cached->clone();
    }

    return r;
}

QList<Package*> DBRepository::findPackages(Package::Status status,
        bool filterByStatus,
        const QString& query, int cat0, int cat1, QString *err) const
{
    // qDebug() << "DBRepository::findPackages.0";

    QString where;
    QList<QVariant> params;

    QStringList keywords = query.toLower().simplified().split(" ",
            QString::SkipEmptyParts);

    for (int i = 0; i < keywords.count(); i++) {
        if (!where.isEmpty())
            where += " AND ";
        where += "FULLTEXT LIKE :FULLTEXT" + QString::number(i);
        params.append(QString("%" + keywords.at(i).toLower() + "%"));
    }
    if (filterByStatus) {
        if (!where.isEmpty())
            where += " AND ";
        if (status == Package::INSTALLED)
            where += "STATUS >= :STATUS";
        else
            where += "STATUS = :STATUS";
        params.append(QVariant((int) status));
    }

    if (cat0 == 0) {
        if (!where.isEmpty())
            where += " AND ";
        where += "CATEGORY0 IS NULL";
    } else if (cat0 > 0) {
        if (!where.isEmpty())
            where += " AND ";
        where += "CATEGORY0 = :CATEGORY0";
        params.append(QVariant((int) cat0));
    }

    if (cat1 == 0) {
        if (!where.isEmpty())
            where += " AND ";
        where += "CATEGORY1 IS NULL";
    } else if (cat1 > 0) {
        if (!where.isEmpty())
            where += " AND ";
        where += "CATEGORY1 = :CATEGORY1";
        params.append(QVariant((int) cat1));
    }

    if (!where.isEmpty())
        where = "WHERE " + where;

    where += " ORDER BY TITLE";

    // qDebug() << "DBRepository::findPackages.1";

    return findPackagesWhere(where, params, err);
}

QStringList DBRepository::getCategories(const QStringList& ids, QString* err)
{
    *err = "";

    QString sql = "SELECT NAME FROM CATEGORY WHERE ID IN (" +
            ids.join(", ") + ")";

    QMySqlQuery q;

    if (!q.prepare(sql))
        *err = DBRepository::toString(q.lastError());

    QStringList r;
    if (err->isEmpty()) {
        if (!q.exec())
            *err = toString(q.lastError());
        else {
            while (q.next()) {
                r.append(q.value(0).toString());
            }
        }
    }

    return r;
}

QList<QStringList> DBRepository::findCategories(Package::Status status,
        bool filterByStatus,
        const QString& query, int level, int cat0, int cat1, QString *err) const
{
    // qDebug() << "DBRepository::findPackages.0";

    QString where;
    QList<QVariant> params;

    QStringList keywords = query.toLower().simplified().split(" ",
            QString::SkipEmptyParts);

    for (int i = 0; i < keywords.count(); i++) {
        if (!where.isEmpty())
            where += " AND ";
        where += "FULLTEXT LIKE :FULLTEXT" + QString::number(i);
        params.append(QString("%" + keywords.at(i).toLower() + "%"));
    }
    if (filterByStatus) {
        if (!where.isEmpty())
            where += " AND ";
        if (status == Package::INSTALLED)
            where += "STATUS >= :STATUS";
        else
            where += "STATUS = :STATUS";
        params.append(QVariant((int) status));
    }

    if (cat0 == 0) {
        if (!where.isEmpty())
            where += " AND ";
        where += "CATEGORY0 IS NULL";
    } else if (cat0 > 0) {
        if (!where.isEmpty())
            where += " AND ";
        where += "CATEGORY0 = :CATEGORY0";
        params.append(QVariant((int) cat0));
    }

    if (cat1 == 0) {
        if (!where.isEmpty())
            where += " AND ";
        where += "CATEGORY1 IS NULL";
    } else if (cat1 > 0) {
        if (!where.isEmpty())
            where += " AND ";
        where += "CATEGORY1 = :CATEGORY1";
        params.append(QVariant((int) cat1));
    }

    if (!where.isEmpty())
        where = "WHERE " + where;

    QString sql = QString("SELECT CATEGORY.ID, COUNT(*), CATEGORY.NAME FROM "
            "PACKAGE LEFT JOIN CATEGORY ON PACKAGE.CATEGORY") +
            QString::number(level) +
            " = CATEGORY.ID " +
            where + " GROUP BY CATEGORY.ID, CATEGORY.NAME "
            "ORDER BY CATEGORY.NAME";

    QMySqlQuery q;

    if (!q.prepare(sql))
        *err = DBRepository::toString(q.lastError());

    if (err->isEmpty()) {
        for (int i = 0; i < params.count(); i++) {
            q.bindValue(i, params.at(i));
        }
    }

    QList<QStringList> r;
    if (err->isEmpty()) {
        if (!q.exec())
            *err = toString(q.lastError());

        while (q.next()) {
            QStringList sl;
            sl.append(q.value(0).toString());
            sl.append(q.value(1).toString());
            sl.append(q.value(2).toString());
            r.append(sl);
        }
    }

    return r;
}

QList<Package*> DBRepository::findPackagesWhere(const QString& where,
        const QList<QVariant>& params,
        QString *err) const
{
    *err = "";

    QList<Package*> r;

    QMySqlQuery q;
    QString sql = "SELECT NAME, TITLE, URL, ICON, "
            "DESCRIPTION, LICENSE, "
            "CATEGORY0, CATEGORY1, CATEGORY2, CATEGORY3, CATEGORY4 "
            "FROM PACKAGE";

    if (!where.isEmpty())
        sql += " " + where;

    if (!q.prepare(sql))
        *err = DBRepository::toString(q.lastError());

    if (err->isEmpty()) {
        for (int i = 0; i < params.count(); i++) {
            q.bindValue(i, params.at(i));
        }
    }

    if (err->isEmpty()) {
        if (!q.exec())
            *err = toString(q.lastError());

        while (q.next()) {
            Package* p = new Package(q.value(0).toString(), q.value(1).toString());
            p->url = q.value(2).toString();
            p->icon = q.value(3).toString();
            p->description = q.value(4).toString();
            p->license = q.value(5).toString();

            int cat0 = q.value(6).toInt();
            int cat1 = q.value(7).toInt();
            int cat2 = q.value(8).toInt();
            int cat3 = q.value(9).toInt();
            int cat4 = q.value(10).toInt();

            int cat = cat4;
            if (cat <= 0)
                cat = cat3;
            if (cat <= 0)
                cat = cat2;
            if (cat <= 0)
                cat = cat1;
            if (cat <= 0)
                cat = cat0;

            if (cat > 0) {
                QString category = findCategory(cat);
                if (!category.isEmpty())
                    p->categories.append(category);
            }

            r.append(p);
        }
    }

    return r;
}

int DBRepository::insertCategory(int parent, int level,
        const QString& category, QString* err)
{
    *err = "";

    QMySqlQuery q;
    QString sql = "SELECT ID FROM CATEGORY WHERE PARENT = :PARENT AND "
            "LEVEL = :LEVEL AND NAME = :NAME";
    q.prepare(sql);
    q.bindValue(":NAME", category);
    q.bindValue(":PARENT", parent);
    q.bindValue(":LEVEL", level);
    q.exec(); // TODO: check result

    int id;
    if (q.next())
        id = q.value(0).toInt();
    else {
        sql = "INSERT INTO CATEGORY "
                "(ID, NAME, PARENT, LEVEL) "
                "VALUES (NULL, :NAME, :PARENT, :LEVEL)";
        q.prepare(sql); // TODO: test return value in all .prepare() calls
        q.bindValue(":NAME", category);
        q.bindValue(":PARENT", parent);
        q.bindValue(":LEVEL", level);
        q.exec();
        id = q.lastInsertId().toInt();
        *err = toString(q.lastError());
    }

    return id;
}

QString DBRepository::savePackage(Package *p, bool replace)
{
    QString err;

    /*
    if (p->name == "com.microsoft.Windows64")
        qDebug() << p->name << "->" << p->description;
        */

    int cat0 = 0;
    int cat1 = 0;
    int cat2 = 0;
    int cat3 = 0;
    int cat4 = 0;

    if (p->categories.count() > 0) {
        QString category = p->categories.at(0);
        QStringList cats = category.split('|');
        for (int i = 0; i < cats.length(); i++) {
            cats[i] = cats.at(i).trimmed();
        }

        QString c;
        if (cats.count() > 0) {
            c = cats.at(0);
            cat0 = insertCategory(0, 0, c, &err);
        }
        if (cats.count() > 1) {
            c = cats.at(1);
            cat1 = insertCategory(cat0, 1, c, &err);
        }
        if (cats.count() > 2) {
            c = cats.at(2);
            cat2 = insertCategory(cat1, 2, c, &err);
        }
        if (cats.count() > 3) {
            c = cats.at(3);
            cat3 = insertCategory(cat2, 3, c, &err);
        }
        if (cats.count() > 4) {
            c = cats.at(4);
            cat4 = insertCategory(cat3, 4, c, &err);
        }
    }

    QMySqlQuery q;
    QString sql = "INSERT OR ";
    if (replace)
        sql += "REPLACE";
    else
        sql += "IGNORE";
    sql += " INTO PACKAGE "
            "(NAME, TITLE, URL, ICON, DESCRIPTION, LICENSE, FULLTEXT, "
            "STATUS, SHORT_NAME, CATEGORY0, CATEGORY1, CATEGORY2, CATEGORY3,"
            " CATEGORY4)"
            "VALUES(:NAME, :TITLE, :URL, :ICON, :DESCRIPTION, :LICENSE, "
            ":FULLTEXT, :STATUS, :SHORT_NAME, "
            ":CATEGORY0, :CATEGORY1, :CATEGORY2, :CATEGORY3, :CATEGORY4)";
    q.prepare(sql);
    q.bindValue(":NAME", p->name);
    q.bindValue(":TITLE", p->title);
    q.bindValue(":URL", p->url);
    q.bindValue(":ICON", p->icon);
    q.bindValue(":DESCRIPTION", p->description);
    q.bindValue(":LICENSE", p->license);
    q.bindValue(":FULLTEXT", (p->title + " " + p->description + " " +
            p->name).toLower());
    q.bindValue(":STATUS", 0);
    q.bindValue(":SHORT_NAME", p->getShortName());
    if (cat0 == 0)
        q.bindValue(":CATEGORY0", QVariant(QVariant::Int));
    else
        q.bindValue(":CATEGORY0", cat0);
    if (cat1 == 0)
        q.bindValue(":CATEGORY1", QVariant(QVariant::Int));
    else
        q.bindValue(":CATEGORY1", cat1);
    if (cat2 == 0)
        q.bindValue(":CATEGORY2", QVariant(QVariant::Int));
    else
        q.bindValue(":CATEGORY2", cat2);
    if (cat3 == 0)
        q.bindValue(":CATEGORY3", QVariant(QVariant::Int));
    else
        q.bindValue(":CATEGORY3", cat3);
    if (cat4 == 0)
        q.bindValue(":CATEGORY4", QVariant(QVariant::Int));
    else
        q.bindValue(":CATEGORY4", cat4);
    q.exec();
    err = toString(q.lastError());

    return err;
}

QString DBRepository::savePackage(Package *p)
{
    return savePackage(p, true);
}

QString DBRepository::savePackageVersion(PackageVersion *p)
{
    return savePackageVersion(p, true);
}

QList<Package*> DBRepository::findPackagesByShortName(const QString &name)
{
    QList<Package*> r;

    QMySqlQuery q;
    q.prepare("SELECT NAME, TITLE, URL, ICON, "
            "DESCRIPTION, LICENSE FROM PACKAGE WHERE SHORT_NAME = :SHORT_NAME");
    q.bindValue(":SHORT_NAME", name);
    q.exec();
    while (q.next()) {
        Package* p = new Package(q.value(0).toString(), q.value(1).toString());
        p->url = q.value(2).toString();
        p->icon = q.value(3).toString();
        p->description = q.value(4).toString();
        p->license = q.value(5).toString();
        r.append(p);
    }

    // TODO: read category

    return r;
}

QString DBRepository::savePackageVersion(PackageVersion *p, bool replace)
{
    QMySqlQuery q;
    QString sql = "INSERT OR ";
    if (replace)
        sql += "REPLACE";
    else
        sql += "IGNORE";
    sql += " INTO PACKAGE_VERSION "
            "(NAME, PACKAGE, CONTENT, MSIGUID, DETECT_FILE_COUNT)"
            "VALUES(:NAME, :PACKAGE, :CONTENT, :MSIGUID, :DETECT_FILE_COUNT)";
    q.prepare(sql);
    q.bindValue(":NAME", p->version.getVersionString());
    q.bindValue(":PACKAGE", p->package);
    q.bindValue(":MSIGUID", p->msiGUID);
    q.bindValue(":DETECT_FILE_COUNT", p->detectFiles.count());
    QDomDocument doc;
    QDomElement root = doc.createElement("version");
    doc.appendChild(root);
    p->toXML(&root);
    QByteArray file;
    QTextStream s(&file);
    doc.save(s, 4);

    q.bindValue(":CONTENT", QVariant(file));
    q.exec();
    return toString(q.lastError());
}

PackageVersion *DBRepository::findPackageVersionByMSIGUID_(
        const QString &guid, QString* err) const
{
    *err = "";

    PackageVersion* r = 0;

    QMySqlQuery q;
    q.prepare("SELECT NAME, "
            "PACKAGE, CONTENT FROM PACKAGE_VERSION "
            "WHERE MSIGUID = :MSIGUID");
    q.bindValue(":MSIGUID", guid);
    if (!q.exec())
        *err = toString(q.lastError());

    if (err->isEmpty()) {
        if (q.next()) {
            QDomDocument doc;
            int errorLine, errorColumn;
            if (!doc.setContent(q.value(2).toByteArray(),
                    err, &errorLine, &errorColumn))
                *err = QString(
                        QObject::tr("XML parsing failed at line %1, column %2: %3")).
                        arg(errorLine).arg(errorColumn).arg(*err);

            if (err->isEmpty()) {
                QDomElement root = doc.documentElement();
                PackageVersion* p = PackageVersion::parse(&root, err);

                if (err->isEmpty()) {
                    r = p;
                }
            }
        }
    }

    return r;
}

QString DBRepository::clear()
{
    Job* job = new Job();

    this->categories.clear();

    if (job->shouldProceed(QObject::tr("Starting an SQL transaction"))) {
        QString err = exec("BEGIN TRANSACTION");
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(0.01);
    }

    if (job->shouldProceed(QObject::tr("Clearing the packages table"))) {
        QString err = exec("DELETE FROM PACKAGE");
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(0.1);
    }

    if (job->shouldProceed(QObject::tr("Clearing the package versions table"))) {
        QString err = exec("DELETE FROM PACKAGE_VERSION");
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(0.7);
    }

    if (job->shouldProceed(QObject::tr("Clearing the licenses table"))) {
        QString err = exec("DELETE FROM LICENSE");
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(0.96);
    }

    if (job->shouldProceed(QObject::tr("Clearing the categories table"))) {
        QString err = exec("DELETE FROM CATEGORY");
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(0.97);
    }

    if (job->shouldProceed(QObject::tr("Commiting the SQL transaction"))) {
        QString err = exec("COMMIT");
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(1);
    } else {
        exec("ROLLBACK");
    }

    job->complete();

    return "";
}

void DBRepository::updateF5(Job* job)
{
    HRTimer timer(7);

    /*
     * Example: 0 :  0  ms
        1 :  127  ms
        2 :  13975  ms
        3 :  1665  ms
        4 :  0  ms
        5 :  18189  ms
        6 :  4400  ms
     */
    timer.time(0);
    Repository* r = new Repository();
    if (job->shouldProceed(QObject::tr("Clearing the database"))) {
        QString err = clear();
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(0.1);
    }

    timer.time(1);
    if (job->shouldProceed(QObject::tr("Downloading the remote repositories"))) {
        Job* sub = job->newSubJob(0.69);
        r->load(sub, true);
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(sub->getErrorMessage());
        delete sub;
    }

    timer.time(2);
    if (job->shouldProceed(QObject::tr("Filling the local database"))) {
        Job* sub = job->newSubJob(0.06);
        saveAll(sub, r, false);
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(sub->getErrorMessage());
        delete sub;
    }
    timer.time(3);

    // qDebug() << "updateF5.0";

    timer.time(4);
    if (job->shouldProceed(QObject::tr("Refreshing the installation status"))) {
        Job* sub = job->newSubJob(0.1);
        InstalledPackages::getDefault()->refresh(sub);
        if (!sub->getErrorMessage().isEmpty())
            job->setErrorMessage(sub->getErrorMessage());
        delete sub;
    }

    // qDebug() << "updateF5.1";

    timer.time(5);
    if (job->shouldProceed(
            QObject::tr("Updating the status for installed packages in the database"))) {
        updateStatusForInstalled();
        job->setProgress(0.98);
    }

    // qDebug() << "updateF5.2";

    if (job->shouldProceed(
            QObject::tr("Removing packages without versions"))) {
        QString err = exec("DELETE FROM PACKAGE WHERE NOT EXISTS "
                "(SELECT * FROM PACKAGE_VERSION WHERE PACKAGE = PACKAGE.NAME)");
        if (err.isEmpty())
            job->setProgress(1);
        else
            job->setErrorMessage(err);
    }

    delete r;
    timer.time(6);

    // timer.dump();

    job->complete();
}

void DBRepository::saveAll(Job* job, Repository* r, bool replace)
{
    if (job->shouldProceed(QObject::tr("Starting an SQL transaction"))) {
        QString err = exec("BEGIN TRANSACTION");
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(0.01);
    }

    if (job->shouldProceed(QObject::tr("Inserting data in the packages table"))) {
        QString err = savePackages(r, replace);
        if (err.isEmpty())
            job->setProgress(0.6);
        else
            job->setErrorMessage(err);
    }

    if (job->shouldProceed(QObject::tr("Inserting data in the package versions table"))) {
        QString err = savePackageVersions(r, replace);
        if (err.isEmpty())
            job->setProgress(0.95);
        else
            job->setErrorMessage(err);
    }

    if (job->shouldProceed(QObject::tr("Inserting data in the licenses table"))) {
        QString err = saveLicenses(r, replace);
        if (err.isEmpty())
            job->setProgress(0.98);
        else
            job->setErrorMessage(err);
    }

    if (job->shouldProceed(QObject::tr("Commiting the SQL transaction"))) {
        QString err = exec("COMMIT");
        if (!err.isEmpty())
            job->setErrorMessage(err);
        else
            job->setProgress(1);
    } else {
        exec("ROLLBACK");
    }

    job->complete();
}

void DBRepository::updateStatusForInstalled()
{
    QList<InstalledPackageVersion*> pvs = InstalledPackages::getDefault()->getAll();
    QSet<QString> packages;
    for (int i = 0; i < pvs.count(); i++) {
        InstalledPackageVersion* pv = pvs.at(i);
        packages.insert(pv->package);
    }
    qDeleteAll(pvs);
    pvs.clear();

    QList<QString> packages_ = packages.values();
    for (int i = 0; i < packages_.count(); i++) {
        QString package = packages_.at(i);
        updateStatus(package);
    }
}

QString DBRepository::savePackages(Repository* r, bool replace)
{
    QString err;
    for (int i = 0; i < r->packages.count(); i++) {
        Package* p = r->packages.at(i);
        err = savePackage(p, replace);
        if (!err.isEmpty())
            break;
    }

    return err;
}

QString DBRepository::saveLicenses(Repository* r, bool replace)
{
    QString err;
    for (int i = 0; i < r->licenses.count(); i++) {
        License* p = r->licenses.at(i);
        err = saveLicense(p, replace);
        if (!err.isEmpty())
            break;
    }

    return err;
}

QString DBRepository::savePackageVersions(Repository* r, bool replace)
{
    QString err;
    for (int i = 0; i < r->packageVersions.count(); i++) {
        PackageVersion* p = r->packageVersions.at(i);
        err = savePackageVersion(p, replace);
        if (!err.isEmpty())
            break;
    }

    return err;
}

QString DBRepository::toString(const QSqlError& e)
{
    return e.type() == QSqlError::NoError ? "" : e.text();
}

QString DBRepository::updateStatus(const QString& package)
{
    QString err;

    QList<PackageVersion*> pvs = getPackageVersions_(package, &err);
    PackageVersion* newestInstallable = 0;
    PackageVersion* newestInstalled = 0;
    if (err.isEmpty()) {
        for (int j = 0; j < pvs.count(); j++) {
            PackageVersion* pv = pvs.at(j);
            if (pv->installed()) {
                if (!newestInstalled ||
                        newestInstalled->version.compare(pv->version) < 0)
                    newestInstalled = pv;
            }

            if (pv->download.isValid()) {
                if (!newestInstallable ||
                        newestInstallable->version.compare(pv->version) < 0)
                    newestInstallable = pv;
            }
        }
    }

    if (err.isEmpty()) {
        Package::Status status;
        if (newestInstalled) {
            bool up2date = !(newestInstalled && newestInstallable &&
                    newestInstallable->version.compare(
                    newestInstalled->version) > 0);
            if (up2date)
                status = Package::INSTALLED;
            else
                status = Package::UPDATEABLE;
        } else {
            status = Package::NOT_INSTALLED;
        }

        QMySqlQuery q;
        q.prepare("UPDATE PACKAGE "
                "SET STATUS=:STATUS "
                "WHERE NAME=:NAME");
        q.bindValue(":STATUS", status);
        q.bindValue(":NAME", package);
        q.exec();
        err = toString(q.lastError());
    }
    qDeleteAll(pvs);

    return err;
}

QString DBRepository::open()
{
    QString err;

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    QString path(WPMUtils::getShellDir(CSIDL_COMMON_APPDATA));
    path.append("\\Npackd\\Data.db");
    path = QDir::toNativeSeparators(path);
    db.setDatabaseName(path);
    db.open();
    err = toString(db.lastError());

    bool e = false;
    if (err.isEmpty()) {
        e = tableExists(&db, "PACKAGE", &err);
    }

    if (err.isEmpty()) {
        if (!e) {
            // NULL should be stored in CATEGORYx if a package is not
            // categorized
            db.exec("CREATE TABLE PACKAGE(NAME TEXT, "
                    "TITLE TEXT, "
                    "URL TEXT, "
                    "ICON TEXT, "
                    "DESCRIPTION TEXT, "
                    "LICENSE TEXT, "
                    "FULLTEXT TEXT, "
                    "STATUS INTEGER, "
                    "SHORT_NAME TEXT, "
                    "REPOSITORY INTEGER, "
                    "CATEGORY0 INTEGER, "
                    "CATEGORY1 INTEGER, "
                    "CATEGORY2 INTEGER, "
                    "CATEGORY3 INTEGER, "
                    "CATEGORY4 INTEGER"
                    ")");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE INDEX PACKAGE_FULLTEXT ON PACKAGE(FULLTEXT)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE UNIQUE INDEX PACKAGE_NAME ON PACKAGE(NAME)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE INDEX PACKAGE_SHORT_NAME ON PACKAGE(SHORT_NAME)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        e = tableExists(&db, "CATEGORY", &err);
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE TABLE CATEGORY(ID INTEGER PRIMARY KEY ASC, "
                    "NAME TEXT, PARENT INTEGER, LEVEL INTEGER)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE UNIQUE INDEX CATEGORY_ID ON CATEGORY(ID)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        e = tableExists(&db, "PACKAGE_VERSION", &err);
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE TABLE PACKAGE_VERSION(NAME TEXT, "
                    "PACKAGE TEXT, "
                    "CONTENT BLOB, MSIGUID TEXT, DETECT_FILE_COUNT INTEGER)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE INDEX PACKAGE_VERSION_PACKAGE ON PACKAGE_VERSION("
                    "PACKAGE)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE UNIQUE INDEX PACKAGE_VERSION_PACKAGE_NAME ON "
                    "PACKAGE_VERSION("
                    "PACKAGE, NAME)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE INDEX PACKAGE_VERSION_DETECT_FILE_COUNT ON PACKAGE_VERSION("
                    "DETECT_FILE_COUNT)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        e = tableExists(&db, "LICENSE", &err);
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE TABLE LICENSE(NAME TEXT, "
                    "TITLE TEXT, "
                    "DESCRIPTION TEXT, "
                    "URL TEXT"
                    ")");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE UNIQUE INDEX LICENSE_NAME ON LICENSE(NAME)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        e = tableExists(&db, "REPOSITORY", &err);
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE TABLE REPOSITORY(ID INTEGER PRIMARY KEY ASC, "
                    "URL TEXT)");
            err = toString(db.lastError());
        }
    }

    if (err.isEmpty()) {
        if (!e) {
            db.exec("CREATE UNIQUE INDEX REPOSITORY_ID ON REPOSITORY(ID)");
            err = toString(db.lastError());
        }
    }

    return err;
}
