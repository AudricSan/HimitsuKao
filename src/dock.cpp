#include "dock.h"

#ifdef HIMITSUKAO_DOCK

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QMetaObject>
#include <QPointer>
#include <QWidget>

#include <string>

#include <obs-browser/browser-panel.hpp> // vendored from obs-browser (third_party/obs-browser)

#include "plugin-service.h"

namespace {

constexpr const char *kDockId = "himitsukao-dock";

QCef *g_cef = nullptr;
QPointer<QWidget> g_widget; // owned by OBS once added as a dock
bool g_dock_added = false;
bool g_callback_registered = false;

// Same address the phone would use, but on loopback, with the code in the fragment (the page stores it and removes
// it) and the "dock" flag (compact layout) in the query so that it survives the fragment removal.
std::string dock_url()
{
	const PluginServerInfo info = plugin_server_info();
	return "http://127.0.0.1:" + std::to_string(info.configured_port) + "/?dock=1#code=" + info.code;
}

void create_dock()
{
	if (g_dock_added)
		return;
	g_cef = obs_browser_init_panel();
	if (!g_cef) {
		obs_log(LOG_WARNING, "obs-browser is not available: the HimitsuKao dock is skipped "
				     "(the phone page and the filter properties still work)");
		return;
	}
	if (!g_cef->initialized()) {
		g_cef->init_browser();
		g_cef->wait_for_browser_init();
	}
	QCefWidget *widget = g_cef->create_widget(nullptr, dock_url());
	if (!widget) {
		obs_log(LOG_WARNING, "could not create the browser panel: the HimitsuKao dock is skipped");
		return;
	}
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("DockTitle"), static_cast<QWidget *>(widget))) {
		obs_log(LOG_WARNING, "could not add the HimitsuKao dock");
		widget->closeBrowser();
		delete widget;
		return;
	}
	g_widget = widget;
	g_dock_added = true;
	obs_log(LOG_INFO, "HimitsuKao dock added");
}

void remove_dock()
{
	if (!g_dock_added)
		return;
	g_dock_added = false;
	if (g_widget)
		static_cast<QCefWidget *>(g_widget.data())->closeBrowser();
	obs_frontend_remove_dock(kDockId);
	g_widget = nullptr;
}

// Runs on the Qt main thread.
void frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		create_dock();
	else if (event == OBS_FRONTEND_EVENT_EXIT)
		remove_dock();
}

} // namespace

void dock_init()
{
	if (g_callback_registered)
		return;
	obs_frontend_add_event_callback(frontend_event, nullptr);
	g_callback_registered = true;
}

void dock_shutdown()
{
	if (g_callback_registered) {
		obs_frontend_remove_event_callback(frontend_event, nullptr);
		g_callback_registered = false;
	}
	remove_dock();
	g_cef = nullptr; // owned by obs-browser's panel layer; nothing to delete here
}

void dock_refresh_url()
{
	QWidget *w = g_widget.data();
	if (!w)
		return;
	// Hop to the widget's (Qt main) thread; the URL is computed there so it is always current.
	QMetaObject::invokeMethod(
		w,
		[] {
			if (g_widget)
				static_cast<QCefWidget *>(g_widget.data())->setURL(dock_url());
		},
		Qt::QueuedConnection);
}

#else // no dock in this build

void dock_init() {}
void dock_shutdown() {}
void dock_refresh_url() {}

#endif
