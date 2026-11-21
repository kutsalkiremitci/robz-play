
// Use the native entry point instead of SDL's WinMain wrapper.
#ifdef CHIAKI_GUI_ENABLE_SDL_GAMECONTROLLER
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#endif
#ifdef main
#undef main
#endif

// ugly workaround because Windows does weird things and ENOTIME
int real_main(int argc, char *argv[]);
void StartupLogFinish(int code);
int main(int argc, char *argv[])
{
	const int code = real_main(argc, argv);
	StartupLogFinish(code);
	return code;
}

#include <streamsession.h>
#include <settings.h>
#include <host.h>
#include <controllermanager.h>
#include <discoverymanager.h>
#include <qmlmainwindow.h>
#include <robztranslator.h>
#include <QApplication>
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <string>
#endif
#include <QtTypes>

#ifdef CHIAKI_ENABLE_CLI
#include <chiaki-cli.h>
#endif

#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/base64.h>

#include <stdio.h>
#include <string.h>

#ifdef CHIAKI_HAVE_WEBENGINE
#include <QtWebEngineQuick>
#endif

#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QMutex>
#include <QSysInfo>
#include <signal.h>
#include <QMap>
#include <QSettings>
#include <QStringList>
#include <QSurfaceFormat>

Q_DECLARE_METATYPE(ChiakiLogLevel)
Q_DECLARE_METATYPE(ChiakiRegistEventType)

#if defined(CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE) && defined(Q_OS_LINUX)
#include <QtPlugin>
Q_IMPORT_PLUGIN(SDInputContextPlugin)
#endif

#ifdef CHIAKI_ENABLE_CLI
struct CLICommand
{
	int (*cmd)(ChiakiLog *log, int argc, char *argv[]);
};

static const QMap<QString, CLICommand> cli_commands = {
	{ "discover", { chiaki_cli_cmd_discover } },
	{ "wakeup", { chiaki_cli_cmd_wakeup } }
};
#endif

// ---------------------------------------------------------------- Robz Play start-up log
//
// Every launch writes what happens while the application starts - and whatever it reports
// later outside a stream - to %APPDATA%\RobzPlay\RobzPlay\log\robzplay-acilis.log. The launch
// before is kept as robzplay-acilis-onceki.log. A crash is noted there as well, with the module
// it happened in. The stream logs (chiaki_session_*.log) only come to be once a stream starts,
// so a launch that fails before that used to leave nothing behind to look at.
//
// ROBZPLAY_TEST_CRASH=1 makes a launch crash on purpose right after the log is open, to see
// that a crash is recorded.
#ifdef _WIN32
static HANDLE startup_log_file = INVALID_HANDLE_VALUE;
static QtMessageHandler startup_log_previous_handler = nullptr;
static LPTOP_LEVEL_EXCEPTION_FILTER startup_log_previous_filter = nullptr;
static QMutex startup_log_mutex;
static qint64 startup_log_bytes = 0;
static const qint64 STARTUP_LOG_LIMIT = 8 * 1024 * 1024;

static void StartupLogRaw(const char *text, size_t length)
{
	if(startup_log_file == INVALID_HANDLE_VALUE)
		return;
	DWORD written = 0;
	WriteFile(startup_log_file, text, static_cast<DWORD>(length), &written, nullptr);
	startup_log_bytes += written;
}

static void StartupLogLine(const QString &line)
{
	const QByteArray utf8 = (QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz ")) + line + QStringLiteral("\r\n")).toUtf8();
	QMutexLocker locker(&startup_log_mutex);
	if(startup_log_bytes > STARTUP_LOG_LIMIT)
		return;
	StartupLogRaw(utf8.constData(), static_cast<size_t>(utf8.size()));
	if(startup_log_bytes > STARTUP_LOG_LIMIT)
	{
		static const char note[] = "... (günlük 8 MB'a ulaştı; bundan sonrasını yazmıyor, bir çökme yine de yazılır)\r\n";
		StartupLogRaw(note, sizeof(note) - 1);
	}
}

static void StartupLogMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
	static const char *const levels[] = { "ayrıntı", "uyarı", "HATA", "ÖLÜMCÜL HATA", "bilgi" };
	const int index = static_cast<int>(type);
	QString line = QStringLiteral("[%1] ").arg(QString::fromUtf8(index >= 0 && index <= 4 ? levels[index] : "?"));
	if(context.category && strcmp(context.category, "default") != 0)
		line += QString::fromLatin1(context.category) + QStringLiteral(": ");
	StartupLogLine(line + message);
	if(type == QtFatalMsg && startup_log_file != INVALID_HANDLE_VALUE)
		FlushFileBuffers(startup_log_file);
	if(startup_log_previous_handler)
		startup_log_previous_handler(type, context, message);
}

// Runs inside a crash: no Qt, no allocation, no lock - only Win32 and a buffer on the stack.
static LONG WINAPI StartupLogCrashFilter(EXCEPTION_POINTERS *info)
{
	if(startup_log_file != INVALID_HANDLE_VALUE && info && info->ExceptionRecord)
	{
		const EXCEPTION_RECORD *record = info->ExceptionRecord;
		wchar_t module_path[MAX_PATH] = L"?";
		HMODULE module = nullptr;
		if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(record->ExceptionAddress), &module))
			GetModuleFileNameW(module, module_path, MAX_PATH);
		const wchar_t *module_name = wcsrchr(module_path, L'\\');
		module_name = module_name ? module_name + 1 : module_path;
		char module_utf8[MAX_PATH * 3] = "?";
		WideCharToMultiByte(CP_UTF8, 0, module_name, -1, module_utf8, sizeof(module_utf8), nullptr, nullptr);
		const unsigned long long offset = module
			? static_cast<unsigned long long>(reinterpret_cast<const char *>(record->ExceptionAddress) - reinterpret_cast<const char *>(module))
			: 0;
		SYSTEMTIME now;
		GetLocalTime(&now);
		char line[1024];
		const int length = snprintf(line, sizeof(line),
			"%02u:%02u:%02u.%03u ÇÖKTÜ: istisna 0x%08lX, %s+0x%llX içinde. Robz Play burada kapandı.\r\n",
			now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
			static_cast<unsigned long>(record->ExceptionCode), module_utf8, offset);
		if(length > 0)
			StartupLogRaw(line, static_cast<size_t>(length < static_cast<int>(sizeof(line)) ? length : static_cast<int>(sizeof(line)) - 1));
		FlushFileBuffers(startup_log_file);
	}
	return startup_log_previous_filter ? startup_log_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}

static void StartupLogAbort(int)
{
	static const char note[] = "ÇÖKTÜ: program kendini durdurdu (abort). Nedeni hemen üstteki satırlarda.\r\n";
	StartupLogRaw(note, sizeof(note) - 1);
	if(startup_log_file != INVALID_HANDLE_VALUE)
		FlushFileBuffers(startup_log_file);
}

static void StartupLogOpen()
{
	const wchar_t *appdata = _wgetenv(L"APPDATA");
	if(!appdata || !*appdata)
		return;
	const QString dir = QString::fromWCharArray(appdata) + QLatin1Char('\\') + QCoreApplication::organizationName()
		+ QLatin1Char('\\') + QCoreApplication::applicationName() + QStringLiteral("\\log");
	QDir().mkpath(dir);
	const QString path = dir + QStringLiteral("\\robzplay-acilis.log");
	const QString previous = dir + QStringLiteral("\\robzplay-acilis-onceki.log");
	MoveFileExW(reinterpret_cast<const wchar_t *>(path.utf16()), reinterpret_cast<const wchar_t *>(previous.utf16()), MOVEFILE_REPLACE_EXISTING);
	startup_log_file = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
		nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(startup_log_file == INVALID_HANDLE_VALUE)
		return;
	static const char bom[] = "\xEF\xBB\xBF";
	StartupLogRaw(bom, 3);

	std::wstring exe(32768, L'\0');
	exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
	StartupLogLine(QStringLiteral("Robz Play %1 açılıyor (chiaki-ng %2), %3")
		.arg(QStringLiteral(ROBZPLAY_VERSION), QStringLiteral(CHIAKI_VERSION), QDateTime::currentDateTime().toString(Qt::ISODate)));
	StartupLogLine(QStringLiteral("Windows: %1, çekirdek %2").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion()));
	StartupLogLine(QStringLiteral("Program: %1").arg(QString::fromStdWString(exe)));

	startup_log_previous_handler = qInstallMessageHandler(StartupLogMessageHandler);
	startup_log_previous_filter = SetUnhandledExceptionFilter(StartupLogCrashFilter);
	signal(SIGABRT, StartupLogAbort);

	if(qEnvironmentVariableIntValue("ROBZPLAY_TEST_CRASH") == 1)
	{
		StartupLogLine(QStringLiteral("ROBZPLAY_TEST_CRASH: bilerek çöküyor"));
		*static_cast<volatile int *>(nullptr) = 1;
	}
}

void StartupLogFinish(int code)
{
	if(startup_log_file == INVALID_HANDLE_VALUE)
		return;
	StartupLogLine(QStringLiteral("Kapanış: normal, çıkış kodu %1").arg(code));
	FlushFileBuffers(startup_log_file);
}
#else
static void StartupLogOpen() {}
void StartupLogFinish(int) {}
#endif

// Robz Play is a fork of chiaki-ng and keeps its own settings. On the very first
// launch, copy chiaki-ng's saved settings (registered consoles, video and
// controller options, profiles) so an existing chiaki-ng user does not have to
// pair the console again. chiaki-ng's own settings are never modified.
//
// The copy then starts where Robz Play is meant to be used, whatever chiaki-ng was set
// to: frame generation on with a 60 FPS target, a 60 FPS stream on the home network
// (the frame generator is built around the 60 FPS stream, in which a 30 FPS game arrives
// as pairs of equal frames), and the Vulkan renderer. Everything else is taken over as it
// was.
//
// ROBZPLAY_IMPORT_FROM_ORG names another settings organisation to import from; it exists
// so the import can be tried against made-up settings without touching real ones.
static void ImportChiakiNgSettingsOnce()
{
	const QString own_org = QCoreApplication::organizationName();
	const QString own_app = QCoreApplication::applicationName();
	const QString src_org = qEnvironmentVariableIsEmpty("ROBZPLAY_IMPORT_FROM_ORG")
		? QStringLiteral("Chiaki") : qEnvironmentVariable("ROBZPLAY_IMPORT_FROM_ORG");
	const QString src_app = QStringLiteral("Chiaki");
	if(own_org == src_org)
		return;

	QSettings own(own_org, own_app);
	own.setFallbacksEnabled(false);
	if(!own.allKeys().isEmpty())
		return;

	QSettings src(src_org, src_app);
	src.setFallbacksEnabled(false);
	if(src.allKeys().isEmpty())
		return;

	auto copy_all = [](QSettings &from, QSettings &to) {
		const QStringList keys = from.allKeys();
		for(const QString &key : keys)
			to.setValue(key, from.value(key));
		// Connecting to a console the moment the application starts is not carried over:
		// Robz Play opens on its home screen until the user asks for it here
		// (Settings > Consoles > Auto-Connect).
		to.remove("settings/auto_connect_mac");
	};

	auto start_with_robzplay_defaults = [](QSettings &to) {
		// Vulkan is the renderer Robz Play is built and tested on, so a chiaki-ng that was
		// left on OpenGL does not decide it - and neither does a start-up of chiaki-ng's that
		// happened to be cut short. Where Vulkan really cannot start, the application still
		// falls back to OpenGL by itself.
		to.setValue("settings/render_backend", "vulkan");
		to.remove("settings/vulkan_startup_pending");

		to.setValue("settings/framegen_enabled", true);
		to.setValue("settings/framegen_target_fps", 60);
		to.setValue("settings/fps_local_ps5", 60);
		to.setValue("settings/fps_local_ps4", 60);
		// settings written by an old chiaki-ng keep the frame rate under this name until
		// they are migrated on load
		if(to.contains("settings/fps"))
			to.setValue("settings/fps", 60);
	};

	copy_all(src, own);
	start_with_robzplay_defaults(own);

	QStringList profiles;
	int count = src.beginReadArray("profiles");
	for(int i = 0; i < count; i++)
	{
		src.setArrayIndex(i);
		const QString name = src.value("settings/profile_name").toString();
		if(!name.isEmpty())
			profiles.append(name);
	}
	src.endArray();
	for(const QString &profile : std::as_const(profiles))
	{
		QSettings src_profile(src_org, QStringLiteral("%1-%2").arg(src_app, profile));
		src_profile.setFallbacksEnabled(false);
		QSettings own_profile(own_org, QStringLiteral("%1-%2").arg(own_app, profile));
		own_profile.setFallbacksEnabled(false);
		copy_all(src_profile, own_profile);
		start_with_robzplay_defaults(own_profile);
	}

	QSettings src_placebo(src_org, QStringLiteral("pl_render_params"));
	src_placebo.setFallbacksEnabled(false);
	QSettings own_placebo(own_org, QStringLiteral("pl_render_params"));
	own_placebo.setFallbacksEnabled(false);
	copy_all(src_placebo, own_placebo);

	own.setValue("robzplay/imported_from_chiaki_ng", true);
	own.sync();
	fprintf(stderr, "Robz Play: imported existing chiaki-ng settings (%d profile(s))\n", (int)profiles.size());
}

// The settings Robz Play and Robz Vision are built and tested with: the Vulkan renderer (Robz
// Vision cannot capture the OpenGL one), H265, 1080p at 60 FPS with 100 Mbps on the home
// network and over the internet, and the sound at full volume.
//
// They are written into the settings once - for a fresh installation, for settings taken over
// from chiaki-ng and for an update from a version without them alike - and whatever the user
// changes afterwards stays. Raising ROBZPLAY_DEFAULTS_VERSION applies a changed set once more.
static const int ROBZPLAY_DEFAULTS_VERSION = 1;

static void ApplyRobzPlayDefaults(QSettings &s)
{
	// names that chiaki-ng's oldest versions used; on load they would overwrite the values below
	for(const char *legacy : { "settings/resolution", "settings/fps", "settings/codec", "settings/bitrate" })
		s.remove(QString::fromLatin1(legacy));

	s.setValue("settings/render_backend", "vulkan");
	s.remove("settings/vulkan_startup_pending");
	s.remove("settings/vulkan_startup_failures");
	s.remove("settings/vulkan_fallback_version");

	s.setValue("settings/codec_local_ps5", "h265");
	s.setValue("settings/codec_remote_ps5", "h265");
	s.setValue("settings/resolution_local_ps5", "1080p");
	s.setValue("settings/resolution_remote_ps5", "1080p");
	s.setValue("settings/fps_local_ps5", 60);
	s.setValue("settings/fps_remote_ps5", 60);
	s.setValue("settings/bitrate_local_ps5", 100000);    // kbps
	s.setValue("settings/bitrate_remote_ps5", 100000);
	s.setValue("settings/audio_volume", 128);            // SDL_MIX_MAXVOLUME: 100 %
}

static void ApplyRobzPlayDefaultsOnce()
{
	const QString org = QCoreApplication::organizationName();
	const QString app = QCoreApplication::applicationName();
	QSettings own(org, app);
	own.setFallbacksEnabled(false);
	if(own.value("robzplay/defaults_version", 0).toInt() >= ROBZPLAY_DEFAULTS_VERSION)
		return;

	ApplyRobzPlayDefaults(own);

	QStringList profiles;
	const int count = own.beginReadArray("profiles");
	for(int i = 0; i < count; i++)
	{
		own.setArrayIndex(i);
		const QString name = own.value("settings/profile_name").toString();
		if(!name.isEmpty())
			profiles.append(name);
	}
	own.endArray();
	for(const QString &profile : std::as_const(profiles))
	{
		QSettings own_profile(org, QStringLiteral("%1-%2").arg(app, profile));
		own_profile.setFallbacksEnabled(false);
		ApplyRobzPlayDefaults(own_profile);
	}

	own.setValue("robzplay/defaults_version", ROBZPLAY_DEFAULTS_VERSION);
	own.sync();
	fprintf(stderr, "Robz Play: applied the Robz Play defaults (version %d, %d profile(s))\n", ROBZPLAY_DEFAULTS_VERSION, (int)profiles.size());
}

int RunStream(QGuiApplication &app, const StreamSessionConnectInfo &connect_info);
int RunMain(QGuiApplication &app, Settings *settings, bool exit_app_on_stream_exit);

int real_main(int argc, char *argv[])
{
	qRegisterMetaType<DiscoveryHost>();
	qRegisterMetaType<RegisteredHost>();
	qRegisterMetaType<HostMAC>();
	qRegisterMetaType<ChiakiQuitReason>();
	qRegisterMetaType<ChiakiRegistEventType>();
	qRegisterMetaType<ChiakiLogLevel>();

	QGuiApplication::setOrganizationName("RobzPlay");
	// Development aid: ROBZPLAY_SETTINGS_ORG=<name> runs against a separate, empty settings
	// store, so test launches never touch (or auto-connect with) the user's real consoles.
	const bool sandbox_settings = qEnvironmentVariableIsSet("ROBZPLAY_SETTINGS_ORG") && !qEnvironmentVariableIsEmpty("ROBZPLAY_SETTINGS_ORG");
	if(sandbox_settings)
		QGuiApplication::setOrganizationName(qEnvironmentVariable("ROBZPLAY_SETTINGS_ORG"));
	QGuiApplication::setApplicationName("RobzPlay");
	QGuiApplication::setApplicationVersion(ROBZPLAY_VERSION);
	StartupLogOpen();
	QGuiApplication::setApplicationDisplayName("Robz Play");
#if defined(Q_OS_MACOS)
	// Use a plain CAMetalLayer for Vulkan windows. Qt's own layer presents in step with each
	// live resize, but our swapchain presents from the render thread, so every resize step
	// stalled and resizing the window became very slow.
	qputenv("QT_MTL_NO_TRANSACTION", "1");
	// The Vulkan loader also reads Homebrew's driver folder, so with Homebrew's molten-vk installed
	// the app loaded a second MoltenVK beside its own and listed every GPU twice. Use only the
	// bundled one. Setting any of these variables yourself keeps your choice.
	const QString moltenvk_icd = QFileInfo(QString::fromLocal8Bit(argv[0])).canonicalPath()
		+ "/../Resources/vulkan/icd.d/MoltenVK_icd.json";
	if(QFileInfo::exists(moltenvk_icd) && !qEnvironmentVariableIsSet("VK_DRIVER_FILES")
		&& !qEnvironmentVariableIsSet("VK_ICD_FILENAMES") && !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_SELECT")
		&& !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_DISABLE"))
		qputenv("VK_DRIVER_FILES", QFileInfo(moltenvk_icd).canonicalFilePath().toUtf8());
#endif
#if defined(Q_OS_LINUX)
	if(qEnvironmentVariableIsSet("FLATPAK_ID"))
		QGuiApplication::setDesktopFileName(qEnvironmentVariable("FLATPAK_ID"));
	else
#endif
		QGuiApplication::setDesktopFileName("robzplay");

	qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-gpu");
#if defined(Q_OS_WIN)
	// The QML modules ship in a "qml" folder beside the executable, and Qt is told so here.
	// The path of the executable and the variable are both handled as UTF-16. Read from
	// argv[0] and set as UTF-8, as this used to be done, the path was garbled whenever a
	// folder on the way had a letter outside ASCII - a Windows user name such as "bahadır",
	// which is where a per-user installation lives - and the application then closed at
	// once, without a window, finding none of its QML modules.
	{
		std::wstring exe_path(32768, L'\0');
		const DWORD exe_path_len = GetModuleFileNameW(nullptr, exe_path.data(), static_cast<DWORD>(exe_path.size()));
		if(exe_path_len > 0 && exe_path_len < exe_path.size())
		{
			exe_path.resize(exe_path_len);
			const QString import_path = QFileInfo(QString::fromStdWString(exe_path)).dir().absolutePath() + "/qml";
			_wputenv((L"QML_IMPORT_PATH=" + import_path.toStdWString()).c_str());
		}
	}
	// Microsoft's OpenCL, OpenGL and Vulkan Compatibility Pack adds the Dozen driver (Vulkan on
	// D3D12) beside the GPU's own, and loading it can crash the app even when it isn't used. Where
	// Dozen is the only Vulkan driver, Vulkan then finds no device and chiaki-ng uses OpenGL,
	// which the pack also provides. Setting either variable yourself keeps your choice.
	if(!qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_DISABLE") && !qEnvironmentVariableIsSet("VK_LOADER_DRIVERS_SELECT"))
		qputenv("VK_LOADER_DRIVERS_DISABLE", "*dzn*");
#endif
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
	qputenv("ANV_VIDEO_DECODE", "1");
	qputenv("RADV_PERFTEST", "video_decode");
#endif
#ifdef CHIAKI_GUI_ENABLE_STEAMDECK_NATIVE
	if (qEnvironmentVariableIsSet("SteamDeck"))
		qputenv("QT_IM_MODULE", "sdinput");
#endif

	ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "Chiaki lib init failed: %s\n", chiaki_error_string(err));
		return 1;
	}

    SDL_SetHint(SDL_HINT_APP_NAME, "Robz Play");

	if(SDL_Init(SDL_INIT_AUDIO) < 0)
	{
		fprintf(stderr, "SDL Audio init failed: %s\n", SDL_GetError());
		return 1;
	}

	QGuiApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#ifdef CHIAKI_HAVE_WEBENGINE
	QtWebEngineQuick::initialize();
#endif
	QApplication app(argc, argv);

#ifdef Q_OS_MACOS
	QGuiApplication::setWindowIcon(QIcon(":/icons/chiaking_macos.svg"));
#else
	QGuiApplication::setWindowIcon(QIcon(":/icons/robzplay.svg"));
#endif

	QCommandLineParser parser;
	parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
	parser.addHelpOption();
	
	QStringList cmds;
	cmds.append("stream");
	cmds.append("list");
#ifdef CHIAKI_ENABLE_CLI
	cmds.append(cli_commands.keys());
#endif

	parser.addPositionalArgument("command", cmds.join(", "));
	parser.addPositionalArgument("nickname", "Needed for stream command to get credentials for connecting. "
			"Use 'list' to get the nickname.");
	parser.addPositionalArgument("host", "Address to connect to (when using the stream command).");

	QCommandLineOption profile_option("profile", "", "profile", "Configuration profile");
	parser.addOption(profile_option);

	QCommandLineOption stream_exit_option("exit-app-on-stream-exit", "Exit the GUI application when the stream session ends.");
	parser.addOption(stream_exit_option);

	QCommandLineOption regist_key_option("registkey", "", "registkey");
	parser.addOption(regist_key_option);

	QCommandLineOption morning_option("morning", "", "morning");
	parser.addOption(morning_option);

	QCommandLineOption fullscreen_option("fullscreen", "Start window in fullscreen mode [maintains aspect ratio, adds black bars to fill unsused parts of screen if applicable] (only for use with stream command).");
	parser.addOption(fullscreen_option);

	QCommandLineOption dualsense_option("dualsense", "Enable DualSense haptics and adaptive triggers (PS5 and DualSense connected via USB only).");
	parser.addOption(dualsense_option);

	QCommandLineOption zoom_option("zoom", "Start window in fullscreen zoomed in to fit screen [maintains aspect ratio, cutting off edges of image to fill screen] (only for use with stream command)");
	parser.addOption(zoom_option);

	QCommandLineOption stretch_option("stretch", "Start window in fullscreen stretched to fit screen [distorts aspect ratio to fill screen] (only for use with stream command).");
	parser.addOption(stretch_option);

	QCommandLineOption passcode_option("passcode", "Automatically send your PlayStation login passcode (only affects users with a login passcode set on their PlayStation console).", "passcode");
	parser.addOption(passcode_option);

	QCommandLineOption opengl_option("opengl", "Switch the renderer to OpenGL from now on, for systems where Vulkan shows a blank window or crashes (switch back under Settings > Video > Renderer Backend).");
	parser.addOption(opengl_option);

	parser.process(app);
	QStringList args = parser.positionalArguments();

	if(!sandbox_settings || !qEnvironmentVariableIsEmpty("ROBZPLAY_IMPORT_FROM_ORG"))
		ImportChiakiNgSettingsOnce();
	ApplyRobzPlayDefaultsOnce();

	Settings settings(parser.isSet(profile_option) ? parser.value(profile_option) : QString());
	RobzTranslator::instance()->setLanguage(settings.GetLanguage());
	app.installTranslator(RobzTranslator::instance());
	bool exit_app_on_stream_exit = parser.isSet(stream_exit_option);
	if(parser.isSet(profile_option))
		settings.SetCurrentProfile(parser.value(profile_option));
	Settings alt_settings(parser.isSet(profile_option) ? "" : settings.GetCurrentProfile());
	if(!settings.GetCurrentProfile().isEmpty())
		QGuiApplication::setApplicationDisplayName(QString("Robz Play:%1").arg(settings.GetCurrentProfile()));
	bool use_alt_settings = false;
	if(!parser.isSet(profile_option))
		use_alt_settings = true;

	if(parser.isSet(opengl_option))
	{
		Settings *active_settings = use_alt_settings ? &alt_settings : &settings;
		active_settings->SetRenderBackend(RenderBackend::OpenGL);
		active_settings->SetVulkanStartupPending(false);
		active_settings->Sync();
	}

	if(args.length() == 0)
		return RunMain(app, use_alt_settings ? &alt_settings : &settings, exit_app_on_stream_exit);

	if(args[0] == "list")
	{
		for(const auto &host : settings.GetRegisteredHosts())
			printf("Host: %s \n", host.GetServerNickname().toLocal8Bit().constData());
		return 0;
	}
	if(args[0] == "stream")
	{
		if(args.length() < 2)
			parser.showHelp(1);

		//QString host = args[sizeof(args) -1]; //the ip is always the last param for stream
		QString host = args[args.size()-1];
		QByteArray morning;
		QByteArray regist_key;
		QString initial_login_passcode;
		ChiakiTarget target = CHIAKI_TARGET_PS4_10;

		if(parser.value(regist_key_option).isEmpty() && parser.value(morning_option).isEmpty())
		{
			if(args.length() < 3)
				parser.showHelp(1);

			bool found = false;
			for(const auto &temphost : settings.GetRegisteredHosts())
			{
				if(temphost.GetServerNickname() == args[1])
				{
					found = true;
					morning = temphost.GetRPKey();
					regist_key = temphost.GetRPRegistKey();
					target = temphost.GetTarget();
					break;
				}
			}
			if(!found)
			{
				printf("No configuration found for '%s'\n", args[1].toLocal8Bit().constData());
				return 1;
			}
		}
		else
		{
			// TODO: explicit option for target
			regist_key = parser.value(regist_key_option).toUtf8();
			if(regist_key.length() > sizeof(ChiakiConnectInfo::regist_key))
			{
				printf("Given regist key is too long (expected size <=%llu, got %" PRIdQSIZETYPE")\n",
					(unsigned long long)sizeof(ChiakiConnectInfo::regist_key),
					regist_key.length());
				return 1;
			}
			regist_key += QByteArray(sizeof(ChiakiConnectInfo::regist_key) - regist_key.length(), 0);
			morning = QByteArray::fromBase64(parser.value(morning_option).toUtf8());
			if(morning.length() != sizeof(ChiakiConnectInfo::morning))
			{
				printf("Given morning has invalid size (expected %llu, got %" PRIdQSIZETYPE")\n",
					(unsigned long long)sizeof(ChiakiConnectInfo::morning),
					morning.length());
				printf("Given morning has invalid size (expected %llu)", (unsigned long long)sizeof(ChiakiConnectInfo::morning));
				return 1;
			}
		}
		if ((parser.isSet(stretch_option) && (parser.isSet(zoom_option) || parser.isSet(fullscreen_option))) || (parser.isSet(zoom_option) && parser.isSet(fullscreen_option)))
		{
			printf("Must choose between fullscreen, zoom or stretch option.");
			return 1;
		}
		if(parser.value(passcode_option).isEmpty())
		{
			//Set to empty if it wasn't given by user.
			initial_login_passcode = QString("");
		}
		else
		{
			initial_login_passcode = parser.value(passcode_option);
			if(initial_login_passcode.length() != 4)
			{
				printf("Login passcode must be 4 digits. You entered %" PRIdQSIZETYPE "digits)\n", initial_login_passcode.length());
				return 1;
			}
		}
		
		StreamSessionConnectInfo connect_info(
				use_alt_settings ? &alt_settings : &settings,
				target,
				std::move(host),
				QString(),
				std::move(regist_key),
				std::move(morning),
				std::move(initial_login_passcode),
				QString(),
				false,
				parser.isSet(fullscreen_option),
				parser.isSet(zoom_option),
				parser.isSet(stretch_option));

		return RunStream(app, connect_info);
	}
#ifdef CHIAKI_ENABLE_CLI
	else if(cli_commands.contains(args[0]))
	{
		ChiakiLog log;
		// TODO: add verbose arg
		chiaki_log_init(&log, CHIAKI_LOG_ALL & ~CHIAKI_LOG_VERBOSE, chiaki_log_cb_print, nullptr);

		const auto &cmd = cli_commands[args[0]];
		int sub_argc = args.count();
		QVector<QByteArray> sub_argv_b(sub_argc);
		QVector<char *> sub_argv(sub_argc);
		for(size_t i=0; i<sub_argc; i++)
		{
			sub_argv_b[i] = args[i].toLocal8Bit();
			sub_argv[i] = sub_argv_b[i].data();
		}
		return cmd.cmd(&log, sub_argc, sub_argv.data());
	}
#endif
	else
	{
		parser.showHelp(1);
	}
}

int RunMain(QGuiApplication &app, Settings *settings, bool exit_app_on_stream_exit)
{
	QmlMainWindow main_window(settings, exit_app_on_stream_exit);
	main_window.show();
	return app.exec();
}

int RunStream(QGuiApplication &app, const StreamSessionConnectInfo &connect_info)
{
	QmlMainWindow main_window(connect_info);
	main_window.show();
	return app.exec();
}
