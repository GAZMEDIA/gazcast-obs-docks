/*
GazCast docks for OBS Studio
Copyright (C) 2026 GazCast

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * The direct link between OBS and the GazCast app, with no OBS WebSocket.
 *
 * About once a second the plugin POSTs to http://127.0.0.1:<port>/api/obs/check-in
 * with OBS's scenes and browser sources and the jobs it has finished, and gets
 * new jobs back: add a GazCast overlay to a scene as a browser source, or
 * remove one. If the app isn't running, the request just fails quietly.
 */

#include "obs-link.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <plugin-support.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <cstring>

namespace {

constexpr int kIntervalMs = 1000;
constexpr int kTimeoutMs = 3000;
constexpr const char *kBrowserSource = "browser_source";

QPointer<QNetworkAccessManager> g_net;
QPointer<QTimer> g_timer;
QPointer<QNetworkReply> g_reply;
int g_port = 0;
QJsonArray g_done; /* finished jobs, sent with the next check-in */
bool g_wasConnected = false;

QString Str(const char *s)
{
	return QString::fromUtf8(s ? s : "");
}

QJsonArray SceneNames()
{
	QJsonArray names;
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		names.append(Str(obs_source_get_name(scenes.sources.array[i])));
	}
	obs_frontend_source_list_free(&scenes);
	return names;
}

QString CurrentScene()
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	const QString name = Str(obs_source_get_name(scene));
	obs_source_release(scene);
	return name;
}

QJsonArray BrowserSources()
{
	QJsonArray list;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (strcmp(obs_source_get_unversioned_id(source), kBrowserSource) != 0) {
				return true;
			}
			obs_data_t *settings = obs_source_get_settings(source);
			QJsonObject s;
			s["name"] = Str(obs_source_get_name(source));
			s["url"] = Str(obs_data_get_string(settings, "url"));
			obs_data_release(settings);
			static_cast<QJsonArray *>(param)->append(s);
			return true;
		},
		&list);
	return list;
}

/* "GazCast · Pastor" taken? Try "GazCast · Pastor 2", 3, ... */
QString FreeName(const QString &wanted)
{
	QString name = wanted;
	for (int i = 2;; i++) {
		obs_source_t *taken = obs_get_source_by_name(name.toUtf8().constData());
		if (!taken) {
			return name;
		}
		obs_source_release(taken);
		name = QStringLiteral("%1 %2").arg(wanted).arg(i);
	}
}

/* Returns an empty string on success, otherwise why it failed. */
QString AddSource(const QString &sceneName, const QString &wantedName, const QString &url)
{
	const QUrl u(url);
	/* only the app's own overlay pages on this computer */
	if (u.scheme() != QLatin1String("http") || u.host() != QLatin1String("127.0.0.1") ||
	    !u.path().startsWith(QLatin1String("/o/"))) {
		return QStringLiteral("not a GazCast overlay address");
	}

	if (!obs_source_get_display_name(kBrowserSource)) {
		return QStringLiteral("this OBS has no Browser source (obs-browser is missing)");
	}

	obs_source_t *sceneSource = obs_get_source_by_name(sceneName.toUtf8().constData());
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		return QStringLiteral("there is no scene called \"%1\"").arg(sceneName);
	}

	struct obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	const uint32_t width = ovi.base_width ? ovi.base_width : 1920;
	const uint32_t height = ovi.base_height ? ovi.base_height : 1080;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "url", url.toUtf8().constData());
	obs_data_set_int(settings, "width", 1920);
	obs_data_set_int(settings, "height", 1080);
	obs_data_set_bool(settings, "shutdown", false);
	obs_data_set_bool(settings, "restart_when_active", false);

	const QString name = FreeName(wantedName);
	obs_source_t *source = obs_source_create(kBrowserSource, name.toUtf8().constData(), settings, nullptr);
	obs_data_release(settings);
	if (!source) {
		obs_source_release(sceneSource);
		return QStringLiteral("OBS could not make a browser source (is obs-browser installed?)");
	}

	obs_sceneitem_t *item = obs_scene_add(scene, source);
	if (item) {
		/* The overlays are drawn for 1920 x 1080; fit them to any canvas size. */
		struct vec2 bounds;
		vec2_set(&bounds, static_cast<float>(width), static_cast<float>(height));
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds(item, &bounds);
	}
	obs_source_release(source);
	obs_source_release(sceneSource);

	if (!item) {
		return QStringLiteral("OBS could not put it in \"%1\"").arg(sceneName);
	}
	obs_log(LOG_INFO, "added \"%s\" to scene \"%s\"", name.toUtf8().constData(),
		sceneName.toUtf8().constData());
	return QString();
}

QString RemoveSource(const QString &name)
{
	obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
	if (!source) {
		return QStringLiteral("there is no source called \"%1\"").arg(name);
	}
	if (strcmp(obs_source_get_unversioned_id(source), kBrowserSource) != 0) {
		obs_source_release(source);
		return QStringLiteral("\"%1\" isn't a browser source").arg(name);
	}
	obs_source_remove(source);
	obs_source_release(source);
	obs_log(LOG_INFO, "removed \"%s\"", name.toUtf8().constData());
	return QString();
}

void RunJobs(const QJsonArray &jobs)
{
	for (const QJsonValue &v : jobs) {
		const QJsonObject job = v.toObject();
		const QString kind = job["kind"].toString();
		QString error;
		if (kind == QLatin1String("add")) {
			error = AddSource(job["scene"].toString(), job["name"].toString(), job["url"].toString());
		} else if (kind == QLatin1String("remove")) {
			error = RemoveSource(job["name"].toString());
		} else {
			error = QStringLiteral("this GazCast plugin is too old for that; please update it");
		}

		QJsonObject done;
		done["id"] = job["id"];
		done["ok"] = error.isEmpty();
		if (!error.isEmpty()) {
			done["error"] = error;
			obs_log(LOG_WARNING, "job %d failed: %s", job["id"].toInt(), error.toUtf8().constData());
		}
		g_done.append(done);
	}
}

void CheckIn()
{
	if (!g_net || g_reply) {
		return; /* the last check-in is still on its way */
	}

	QJsonObject body;
	body["version"] = QString::fromUtf8(PLUGIN_VERSION);
	body["scenes"] = SceneNames();
	body["current"] = CurrentScene();
	body["sources"] = BrowserSources();
	body["done"] = g_done;

	QNetworkRequest req(QUrl(QStringLiteral("http://127.0.0.1:%1/api/obs/check-in").arg(g_port)));
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	req.setTransferTimeout(kTimeoutMs);

	const QJsonArray sending = g_done;
	QNetworkReply *reply = g_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	g_reply = reply;
	QObject::connect(reply, &QNetworkReply::finished, reply, [reply, sending]() {
		/* not left to deleteLater: a reply that finishes while OBS is still
		 * starting up can wait a long time for its deferred delete */
		g_reply = nullptr;
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			if (g_wasConnected) {
				obs_log(LOG_INFO, "lost the GazCast app (%s)", reply->errorString().toUtf8().constData());
				g_wasConnected = false;
			}
			return; /* app closed: keep the finished jobs for next time */
		}
		if (!g_wasConnected) {
			obs_log(LOG_INFO, "connected to the GazCast app on port %d", g_port);
			g_wasConnected = true;
		}
		/* the app has these now */
		for (qsizetype i = 0; i < sending.size(); i++) {
			g_done.removeFirst();
		}
		const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
		RunJobs(answer["jobs"].toArray());
		if (!g_done.isEmpty()) {
			QTimer::singleShot(0, CheckIn); /* say so straight away */
		}
	});
}

} // namespace

void ObsLinkStart(int port)
{
	g_port = port;
	if (g_timer) {
		return;
	}

	g_net = new QNetworkAccessManager();
	/* 127.0.0.1 is always this computer; never go through a system proxy */
	g_net->setProxy(QNetworkProxy::NoProxy);

	g_timer = new QTimer();
	g_timer->setInterval(kIntervalMs);
	QObject::connect(g_timer, &QTimer::timeout, g_timer, CheckIn);
	g_timer->start();
}

void ObsLinkSetPort(int port)
{
	g_port = port;
}

void ObsLinkStop()
{
	if (g_timer) {
		g_timer->stop();
		delete g_timer;
	}
	if (g_reply) {
		g_reply->abort();
	}
	if (g_net) {
		delete g_net;
	}
}
