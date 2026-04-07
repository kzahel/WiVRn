#include "hostname.h"
#include "wivrn_config.h"
#include <limits.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "util/u_logging.h"

#if WIVRN_USE_DBUS_CONTROL
#include <gio/gio.h>
#endif

#ifndef HOST_NAME_MAX
#ifdef MAXHOSTNAMELEN
#define HOST_NAME_MAX MAXHOSTNAMELEN
#else
#define HOST_NAME_MAX 256
#endif
#endif

static std::string _hostname()
{
#if WIVRN_USE_DBUS_CONTROL
	GError * error = NULL;
	GDBusConnection * con = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);

	if (!con)
	{
		U_LOG_W("Failed to connect to system bus: %s", error->message);
		g_error_free(error);
	}
	else
	{
		for (auto property: {"PrettyHostname", "StaticHostname", "Hostname"})
		{
			GVariant * result = g_dbus_connection_call_sync(con,
			                                                "org.freedesktop.hostname1",
			                                                "/org/freedesktop/hostname1",
			                                                "org.freedesktop.DBus.Properties",
			                                                "Get",
			                                                g_variant_new("(ss)", "org.freedesktop.hostname1", property),
			                                                G_VARIANT_TYPE("(v)"),
			                                                G_DBUS_CALL_FLAGS_NONE,
			                                                -1,
			                                                NULL,
			                                                &error);

			if (error)
			{
				g_error_free(error);
				error = NULL;
				continue;
			}

			GVariant * property_value;
			g_variant_get(result, "(v)", &property_value);
			const char * hostname = g_variant_get_string(property_value, NULL);

			if (hostname && strcmp(hostname, ""))
			{
				std::string s = hostname;
				g_variant_unref(property_value);
				g_variant_unref(result);
				g_object_unref(con);
				return s;
			}

			g_variant_unref(property_value);
			g_variant_unref(result);
		}

		g_object_unref(con);
	}
#endif

	char buf[HOST_NAME_MAX]{};
#if defined(_WIN32)
	DWORD size = sizeof(buf);
	if (GetComputerNameExA(ComputerNameDnsHostname, buf, &size) != 0 && buf[0] != '\0')
		return buf;

	size = sizeof(buf);
	if (GetComputerNameA(buf, &size) != 0 && buf[0] != '\0')
		return buf;
#else
	int code = gethostname(buf, sizeof(buf));
	if (code == 0)
		return buf;
#endif

	U_LOG_W("Failed to get hostname");
	return "no-hostname";
}

std::string wivrn::hostname()
{
	// Accessing hostname in child process fails with glib
	static std::string result = _hostname();
	return result;
}
