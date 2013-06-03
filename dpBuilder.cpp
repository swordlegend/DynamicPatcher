﻿// created by i-saint
// distributed under Creative Commons Attribution (CC BY) license.
// https://github.com/i-saint/DynamicObjLoader

#include "DynamicPatcher.h"
#include "dpInternal.h"

dpBuilder::dpBuilder(dpContext *ctx)
    : m_context(ctx)
    , m_build_commands()
    , m_create_console(false)
    , m_build_done(false)
    , m_watchfile_stop(false)
    , m_thread_autobuild(nullptr)
{
    std::string VCVersion;
#if     _MSC_VER==1500
    VCVersion = "9.0";
#elif   _MSC_VER==1600
    VCVersion = "10.0";
#elif   _MSC_VER==1700
    VCVersion = "11.0";
#else
#   error
#endif

    std::string keyName = "SOFTWARE\\Microsoft\\VisualStudio\\SxS\\VC7";
    char value[MAX_PATH];
    DWORD size = MAX_PATH;
    HKEY key;
    LONG retKey = ::RegOpenKeyExA(HKEY_LOCAL_MACHINE, keyName.c_str(), 0, KEY_READ|KEY_WOW64_32KEY, &key);
    LONG retVal = ::RegQueryValueExA(key, VCVersion.c_str(), NULL, NULL, (LPBYTE)value, &size );
    if( retKey==ERROR_SUCCESS && retVal==ERROR_SUCCESS  ) {
        m_vcvars += '"';
        m_vcvars += value;
        m_vcvars += "vcvarsall.bat";
        m_vcvars += '"';
#ifdef _WIN64
        m_vcvars += " amd64";
#else // _WIN64
        m_vcvars += " x86";
#endif // _WIN64
        m_msbuild = m_vcvars;
        m_msbuild += " && ";
        m_msbuild += "msbuild";
    }
    ::RegCloseKey( key );
}

dpBuilder::~dpBuilder()
{
    stopAutoBuild();
    if(m_create_console) {
        ::FreeConsole();
    }
}

void dpBuilder::addLoadPath(const char *path)
{
    std::string tmp = path;
    auto p = dpFind(m_loadpathes, [&](const std::string &s){return s==tmp;});
    if(p==m_loadpathes.end()) { m_loadpathes.push_back(tmp); }
}

void dpBuilder::addSourcePath(const char *path)
{
    char fullpath[MAX_PATH];
    ::GetFullPathNameA(path, MAX_PATH, fullpath, nullptr);

    SourcePath tmp = {fullpath, NULL};
    auto p = dpFind(m_srcpathes, [&](const SourcePath &s){return s.path==tmp.path;});
    if(p==m_srcpathes.end()) { m_srcpathes.push_back(tmp); }
}

void dpBuilder::addMSBuildCommand(const char *msbuild_options)
{
    m_build_commands.push_back(m_msbuild+' '+msbuild_options+" /nologo");
}

void dpBuilder::addBuildCommand(const char *any_command)
{
    m_build_commands.push_back(any_command);
}

static void WatchFile( LPVOID arg )
{
    ((dpBuilder*)arg)->watchFiles();
}

bool dpBuilder::startAutoBuild(bool create_console)
{
    if(!m_thread_autobuild) {
        if(m_build_commands.empty()) {
            dpPrintError("dpStartAutoBuild(): no build commands.\n");
            return false;
        }
        m_create_console = create_console;
        if(create_console) {
            ::AllocConsole();
        }
        m_watchfile_stop = false;
        m_thread_autobuild = (HANDLE)_beginthread( WatchFile, 0, this );
        dpPrintInfo("build thread started\n");
        return true;
    }
    return false;
}

bool dpBuilder::stopAutoBuild()
{
    if(m_thread_autobuild!=nullptr) {
        m_watchfile_stop = true;
        ::WaitForSingleObject(m_thread_autobuild, INFINITE);
        m_thread_autobuild = nullptr;
        dpPrintInfo("build thread stopped\n");
        return true;
    }
    return false;
}


void dpBuilder::watchFiles()
{
    // ファイルの変更を監視、変更が検出されたらビルド開始
    for(size_t i=0; i<m_srcpathes.size(); ++i) {
        m_srcpathes[i].notifier = ::FindFirstChangeNotificationA(m_srcpathes[i].path.c_str(), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    }

    while(!m_watchfile_stop) {
        bool needs_build = false;
        for(size_t i=0; i<m_srcpathes.size(); ++i) {
            if(::WaitForSingleObject(m_srcpathes[i].notifier, 10)==WAIT_OBJECT_0) {
                needs_build = true;
            }
        }
        if(needs_build) {
            build();
            for(size_t i=0; i<m_srcpathes.size(); ++i) {
                // ビルドで大量のファイルに変更が加わっていることがあり、以後の FindFirstChangeNotificationA() はそれを検出してしまう。
                // そうなると永遠にビルドし続けてしまうため、HANDLE ごと作りなおして一度リセットする。
                ::FindCloseChangeNotification(m_srcpathes[i].notifier);
                m_srcpathes[i].notifier = ::FindFirstChangeNotificationA(m_srcpathes[i].path.c_str(), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE);
            }
        }
        else {
            ::Sleep(1000);
        }
    }

    for(size_t i=0; i<m_srcpathes.size(); ++i) {
        ::FindCloseChangeNotification(m_srcpathes[i].notifier);
    }
}

bool dpBuilder::build()
{
    dpPrintInfo("build begin\n");

    for(size_t i=0; i<m_build_commands.size(); ++i) {
        const std::string &command = m_build_commands[i];

        STARTUPINFOA si; 
        PROCESS_INFORMATION pi; 
        memset(&si, 0, sizeof(si)); 
        memset(&pi, 0, sizeof(pi)); 
        si.cb = sizeof(si);
        if(::CreateProcessA(NULL, (LPSTR)command.c_str(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)==TRUE) {
            DWORD exit_code = 0;
            ::WaitForSingleObject(pi.hThread, INFINITE);
            ::WaitForSingleObject(pi.hProcess, INFINITE);
            ::GetExitCodeProcess(pi.hProcess, &exit_code);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            if(exit_code!=0) {
                dpPrintError("build failed\n");
                return false;
            }
        }
    }
    ::Sleep(500); // 終了直後だとファイルの書き込みが終わってないことがあるっぽい？ので少し待つ…

    m_build_done = true;
    dpPrintInfo("build succeeded\n");
    return true;
}

void dpBuilder::update()
{
    if(m_thread_autobuild) {
        if(m_build_done) {
            m_build_done = false;
            reload();
        }
    }
    else {
        reload();
    }
}

size_t dpBuilder::reload()
{
    size_t n = 0;
    for(size_t i=0; i<m_loadpathes.size(); ++i) {
        dpGlob(m_loadpathes[i].c_str(), [&](const std::string &path){
            dpBinary *old = dpGetLoader()->findBinary(path.c_str());
            if(dpBinary *bin = dpGetLoader()->load(path.c_str())) {
                if(bin!=old) { ++n; }
            }
        });
    }
    n += dpGetLoader()->reload();
    dpGetLoader()->link();
    return n;
}

const char* dpBuilder::getVCVars() const
{
    return m_vcvars.c_str();
}