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
 * A thin shell: two OBS docks that show pages served by the GazCast desktop
 * app on http://127.0.0.1:<port>. All product logic lives in the app.
 *
 * Browser widgets come from obs-browser's panel API (browser-panel.hpp), the
 * same API OBS itself uses for "Custom Browser Docks".
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <plugin-support.h>

#include "obs-browser/browser-panel.hpp"

#include <QDesktopServices>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <array>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "GazCast";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Docks for the GazCast desktop app (overlay list and control panel).";
}

namespace {

constexpr int kDefaultPort = 5510;
constexpr const char *kAppUrl = "gazcast://open";
constexpr const char *kCookieStore = "gazcast";
/* obs-browser replaces failed loads with its own error page, loaded as a
 * base64 data: URL (see panel/browser-panel-client.cpp, OnLoadError). */
constexpr const char *kErrorPagePrefix = "data:text/html";

struct DockDef {
	const char *id;
	const char *titleKey;
	const char *path;
};

constexpr std::array<DockDef, 2> kDockDefs = {{
	{"gazcast-list", "GazCast.Dock.List", "/dock"},
	{"gazcast-control", "GazCast.Dock.Control", "/dock/control"},
}};

struct Dock {
	const DockDef *def = nullptr;
	QPointer<QWidget> container;
	QPointer<QLabel> placeholder;
	QPointer<QCefWidget> browser;
	bool added = false;
};

QCef *g_cef = nullptr;
QCefCookieManager *g_cookies = nullptr;
std::array<Dock, kDockDefs.size()> g_docks;
QPointer<QTimer> g_initTimer;
int g_port = kDefaultPort;
bool g_shuttingDown = false;

/* ------------------------------------------------------------------------- */
/* Config: <OBS config>/plugin_config/gazcast-docks/config.json  {"port":5510} */

int LoadPort()
{
	BPtr<char> path = obs_module_config_path("config.json");
	if (!path) {
		return kDefaultPort;
	}

	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	if (!data) {
		/* First run: write the defaults so the file is easy to find and edit. */
		data = obs_data_create();
		obs_data_set_int(data, "port", kDefaultPort);

		BPtr<char> dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
		}
		if (!obs_data_save_json_safe(data, path, "tmp", "bak")) {
			obs_log(LOG_WARNING, "could not write default config to %s", path.Get());
		}
	}

	obs_data_set_default_int(data, "port", kDefaultPort);
	long long port = obs_data_get_int(data, "port");
	obs_data_release(data);

	if (port < 1 || port > 65535) {
		obs_log(LOG_WARNING, "invalid port %lld in config.json, using %d", port, kDefaultPort);
		return kDefaultPort;
	}
	return static_cast<int>(port);
}

/* ------------------------------------------------------------------------- */
/* URLs */

QString AppUrl(const DockDef &def)
{
	return QStringLiteral("http://127.0.0.1:%1%2").arg(g_port).arg(QString::fromUtf8(def.path));
}

/* file:///.../data/obs-plugins/gazcast-docks/waiting.html?to=<encoded app url>
 * The page polls /healthz and then navigates to the app URL itself. */
QString StartUrl(const DockDef &def)
{
	const QString target = AppUrl(def);

	BPtr<char> rel = obs_module_file("waiting.html");
	if (!rel) {
		obs_log(LOG_WARNING, "waiting.html not found in the plugin data folder");
		return target;
	}

	BPtr<char> abs = os_get_abs_path_ptr(rel);
	const QString local = QString::fromUtf8(abs ? abs.Get() : rel.Get());
	const QString page = QUrl::fromLocalFile(local).toString(QUrl::FullyEncoded);
	const QString encodedTarget = QString::fromLatin1(QUrl::toPercentEncoding(target));

	return page + QStringLiteral("?to=") + encodedTarget;
}

/* ------------------------------------------------------------------------- */
/* Docks */

void AttachBrowser(Dock &dock)
{
	if (!g_cef || !dock.container || dock.browser) {
		return;
	}

	const std::string url = StartUrl(*dock.def).toStdString();
	QCefWidget *browser = g_cef->create_widget(dock.container, url, g_cookies);
	if (!browser) {
		obs_log(LOG_ERROR, "obs-browser failed to create a widget for %s", dock.def->id);
		return;
	}

	/* Popups (target=_blank, window.open) are not allowed to become CEF
	 * windows; obs-browser then opens them with QDesktopServices instead,
	 * which is how the "Start GazCast" (gazcast://open) link reaches the OS. */
	browser->allowAllPopups(false);

	/* Plain (non-popup) navigations to gazcast://open are also handed to the
	 * OS. obs-browser compares the URL case-insensitively and exactly. */
	g_cef->add_force_popup_url(kAppUrl, dock.container);
	g_cef->add_force_popup_url(std::string(kAppUrl) + "/", dock.container);

	/* If the app goes away later, obs-browser shows its Chromium-style error
	 * page. Swap that for our waiting page, which reconnects by itself. */
	const DockDef *def = dock.def;
	QPointer<QCefWidget> guard(browser);
	QObject::connect(browser, &QCefWidget::urlChanged, dock.container, [def, guard](const QString &url) {
		if (g_shuttingDown || !guard || !url.startsWith(QLatin1String(kErrorPagePrefix))) {
			return;
		}
		obs_log(LOG_INFO, "%s: app not reachable, showing the waiting page", def->id);
		guard->setURL(StartUrl(*def).toStdString());
	});

	if (dock.placeholder) {
		delete dock.placeholder;
	}

	dock.container->layout()->addWidget(browser);
	dock.browser = browser;
}

void AttachBrowsers()
{
	if (g_shuttingDown || !g_cef) {
		return;
	}

	if (!g_cookies) {
		/* A dedicated, persistent request context. obs-browser creates it
		 * under <OBS config>/plugin_config/obs-browser/gazcast. Note that
		 * current obs-browser ignores persist_session_cookies, so the app's
		 * sign-in cookie must carry Expires/Max-Age to survive a restart. */
		g_cookies = g_cef->create_cookie_manager(kCookieStore, true);
		if (!g_cookies) {
			obs_log(LOG_WARNING, "could not create the '%s' cookie store; using the shared one",
				kCookieStore);
		}
	}

	for (Dock &dock : g_docks) {
		AttachBrowser(dock);
	}
}

void CreateDockContainers()
{
	for (size_t i = 0; i < kDockDefs.size(); i++) {
		Dock &dock = g_docks[i];
		dock.def = &kDockDefs[i];

		auto *container = new QWidget();
		container->setObjectName(QString::fromUtf8(dock.def->id) + QStringLiteral("-container"));
		container->setMinimumSize(200, 150);
		/* OBS sets this on its own BrowserDock; CEF needs a native parent window. */
		container->setAttribute(Qt::WA_NativeWindow);

		auto *layout = new QVBoxLayout(container);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);

		auto *label = new QLabel(
			QString::fromUtf8(obs_module_text(g_cef ? "GazCast.Loading" : "GazCast.NoBrowser")), container);
		label->setAlignment(Qt::AlignCenter);
		label->setWordWrap(true);
		label->setMargin(12);
		layout->addWidget(label);

		dock.container = container;
		dock.placeholder = label;

		/* Docks are added from obs_module_post_load, i.e. before OBS restores
		 * the saved window layout, so their position/visibility persist. */
		dock.added = obs_frontend_add_dock_by_id(dock.def->id, obs_module_text(dock.def->titleKey), container);
		if (!dock.added) {
			obs_log(LOG_WARNING, "dock id '%s' already in use", dock.def->id);
			delete container;
		}
	}
}

void StartBrowserWhenReady()
{
	if (!g_cef) {
		return;
	}

	/* Same sequence as OBSBasic::InitBrowserPanelSafeBlock(): init_browser()
	 * returns true when CEF is already up, otherwise it starts it. Instead of
	 * blocking the UI thread with wait_for_browser_init(), poll initialized(). */
	if (g_cef->init_browser()) {
		AttachBrowsers();
		return;
	}

	auto *timer = new QTimer();
	timer->setInterval(100);
	QObject::connect(timer, &QTimer::timeout, timer, [timer]() {
		if (g_shuttingDown) {
			timer->stop();
			timer->deleteLater();
			return;
		}
		if (g_cef && g_cef->initialized()) {
			timer->stop();
			timer->deleteLater();
			AttachBrowsers();
		}
	});
	g_initTimer = timer;
	timer->start();
}

void ReloadDocks()
{
	g_port = LoadPort();
	obs_log(LOG_INFO, "reloading docks (port %d)", g_port);

	for (Dock &dock : g_docks) {
		if (dock.browser) {
			dock.browser->setURL(StartUrl(*dock.def).toStdString());
		}
	}
}

void OpenApp()
{
	if (QDesktopServices::openUrl(QUrl(QString::fromLatin1(kAppUrl)))) {
		return;
	}

	obs_log(LOG_WARNING, "could not open %s (is GazCast installed?)", kAppUrl);
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QMessageBox::warning(main, QString::fromUtf8(obs_module_text("GazCast.NotInstalled.Title")),
			     QString::fromUtf8(obs_module_text("GazCast.NotInstalled.Text")));
}

void Shutdown()
{
	if (g_shuttingDown) {
		return;
	}
	g_shuttingDown = true;

	if (g_initTimer) {
		g_initTimer->stop();
		delete g_initTimer;
	}

	/* Like OBS does for its own browser docks, tear the CEF widgets down while
	 * obs-browser (and CEF) are still alive, before the main window goes. */
	for (Dock &dock : g_docks) {
		if (dock.browser) {
			dock.browser->closeBrowser();
		}
		if (dock.added) {
			obs_frontend_remove_dock(dock.def->id);
			dock.added = false;
		}
	}

	if (g_cookies) {
		g_cookies->FlushStore();
		delete g_cookies;
		g_cookies = nullptr;
	}

	delete g_cef;
	g_cef = nullptr;
}

void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT) {
		Shutdown();
	}
}

} // namespace

/* ------------------------------------------------------------------------- */

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

/* Runs after every module (including obs-browser) is loaded and before OBS
 * restores the dock layout. */
void obs_module_post_load(void)
{
	g_port = LoadPort();

	g_cef = obs_browser_init_panel();
	if (!g_cef) {
		obs_log(LOG_WARNING, "obs-browser panels unavailable; docks will show a notice");
	}

	CreateDockContainers();

	obs_frontend_add_tools_menu_item(obs_module_text("GazCast.Menu.OpenApp"), [](void *) { OpenApp(); }, nullptr);
	obs_frontend_add_tools_menu_item(
		obs_module_text("GazCast.Menu.ReloadDocks"), [](void *) { ReloadDocks(); }, nullptr);

	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);

	StartBrowserWhenReady();

	obs_log(LOG_INFO, "docks ready, app expected on http://127.0.0.1:%d", g_port);
}

void obs_module_unload(void)
{
	/* After OBS_FRONTEND_EVENT_EXIT the frontend API is already torn down
	 * (OBSBasic::closeEvent clears its callbacks), so only unregister if we
	 * are unloaded without having seen EXIT. */
	if (!g_shuttingDown) {
		obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	}
	obs_log(LOG_INFO, "plugin unloaded");
}
