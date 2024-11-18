/***************************************************************************
 * QGeoView is a Qt / C ++ widget for visualizing geographic data.
 * Copyright (C) 2018-2024 Andrey Yaroshenko.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see https://www.gnu.org/licenses.
 ****************************************************************************/

#include "QGVLayerTilesOnline.h"
#include "Raster/QGVImage.h"

#include <QSqlQuery>
#include <QSqlError>

QGVLayerTilesOnline::QGVLayerTilesOnline()
{
    mSqlDatabase.setDatabaseName("tiles_cache.db");

    if(!mSqlDatabase.open()) {
        qgvCritical() << "ERROR" << mSqlDatabase.lastError().text();
    }
    else {
        QSqlQuery query(mSqlDatabase);
        if (!query.exec("CREATE TABLE IF NOT EXISTS Tiles ("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                        "zoom_level INTEGER NOT NULL, "
                        "tile_x INTEGER NOT NULL, "
                        "tile_y INTEGER NOT NULL, "
                        "tile_provider TEXT(0) NOT NULL, "
                        "tile_data BLOB NOT NULL, "
                        "UNIQUE(zoom_level, tile_x, tile_y, tile_provider));")) {
            qgvCritical() << "ERROR" << query.lastError().text();
        }
    }
}

QGVLayerTilesOnline::~QGVLayerTilesOnline()
{
    qDeleteAll(mRequest);
}

void QGVLayerTilesOnline::request(const QGV::GeoTilePos& tilePos)
{
    Q_ASSERT(QGV::getNetworkManager());

    const QUrl url(tilePosToUrl(tilePos));

    auto cachedTile = findCachedTile(tilePos, tilePosToUrl(tilePos));

    if (!cachedTile.isEmpty()) {
        auto tile = new QGVImage();
        tile->setGeometry(tilePos.toGeoRect());
        tile->loadImage(cachedTile);
        tile->setProperty("drawDebug",
                          QString("%1\ntile(%2,%3,%4)")
                                  .arg(url.toString() + " Cached Tile")
                                  .arg(tilePos.zoom())
                                  .arg(tilePos.pos().x())
                                  .arg(tilePos.pos().y()));

        onTile(tilePos, tile);
        return;
    }

    QNetworkRequest request(url);
    QSslConfiguration conf = request.sslConfiguration();
    conf.setPeerVerifyMode(QSslSocket::VerifyNone);

    request.setSslConfiguration(conf);
    request.setRawHeader("User-Agent",
                         "Mozilla/5.0 (Windows; U; MSIE "
                         "6.0; Windows NT 5.1; SV1; .NET "
                         "CLR 2.0.50727)");
    request.setAttribute(QNetworkRequest::HttpPipeliningAllowedAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);

    QNetworkReply* reply = QGV::getNetworkManager()->get(request);

    mRequest[tilePos] = reply;
    connect(reply, &QNetworkReply::finished, reply, [this, reply, tilePos]() { onReplyFinished(reply, tilePos); });

    qgvDebug() << "request" << url;
}

void QGVLayerTilesOnline::cancel(const QGV::GeoTilePos& tilePos)
{
    removeReply(tilePos);
}

void QGVLayerTilesOnline::onReplyFinished(QNetworkReply* reply, const QGV::GeoTilePos& tilePos)
{
    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() != QNetworkReply::OperationCanceledError) {
            qgvCritical() << "ERROR" << reply->errorString();
        }
        removeReply(tilePos);
        return;
    }
    const auto rawImage = reply->readAll();
    auto tile = new QGVImage();
    tile->setGeometry(tilePos.toGeoRect());
    tile->loadImage(rawImage);
    tile->setProperty("drawDebug",
                      QString("%1\ntile(%2,%3,%4)")
                              .arg(reply->url().toString())
                              .arg(tilePos.zoom())
                              .arg(tilePos.pos().x())
                              .arg(tilePos.pos().y()));
    removeReply(tilePos);
    onTile(tilePos, tile);
    cacheTile(rawImage, tilePos, reply->url().toString());
}

void QGVLayerTilesOnline::removeReply(const QGV::GeoTilePos& tilePos)
{
    QNetworkReply* reply = mRequest.value(tilePos, nullptr);
    if (reply == nullptr) {
        return;
    }
    mRequest.remove(tilePos);
    reply->abort();
    reply->close();
    reply->deleteLater();
}

QByteArray QGVLayerTilesOnline::findCachedTile(const QGV::GeoTilePos& tilePos, const QString& url)
{
    QSqlQuery query(mSqlDatabase);
    query.prepare("SELECT tile_data FROM Tiles WHERE "
                  "zoom_level = :zoom AND "
                  "tile_x = :x AND "
                  "tile_y = :y AND "
                  "tile_provider = :provider;");
    query.bindValue(":provider", url.split('/')[2]);
    query.bindValue(":zoom", tilePos.zoom());
    query.bindValue(":x", tilePos.pos().x());
    query.bindValue(":y", tilePos.pos().y());

    if(query.exec() && query.next()) {
        qgvDebug() << "sql query" << query.executedQuery();

        return query.value(0).toByteArray();
    }

    return QByteArray();
}

void QGVLayerTilesOnline::cacheTile(const QByteArray& image, const QGV::GeoTilePos& tilePos, const QString& url)
{
    QSqlQuery query(mSqlDatabase);
    query.prepare("INSERT OR REPLACE INTO Tiles "
                  "(zoom_level, tile_x, tile_y, tile_provider, tile_data) "
                  "VALUES (:zoom, :x, :y, :provider, :data);");
    query.bindValue(":provider", url.split('/')[2]);
    query.bindValue(":zoom", tilePos.zoom());
    query.bindValue(":x", tilePos.pos().x());
    query.bindValue(":y", tilePos.pos().y());
    query.bindValue(":data", image);

    if(!query.exec()) {
        qgvCritical() << "ERROR" << query.lastError().text();
    }

    qgvDebug() << "sql query" << query.executedQuery();
}
