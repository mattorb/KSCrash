//
//  KSCrashC.c
//
//  Created by Karl Stenerud on 2012-01-28.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//


#include "KSCrashC.h"

#include "KSCrashCachedData.h"
#include "KSCrashReport.h"
#include "KSCrashReportFixer.h"
#include "KSCrashReportStore.h"
#include "KSCrashMonitor_CPPException.h"
#include "KSCrashMonitor_Deadlock.h"
#include "KSCrashMonitor_User.h"
#include "KSFileUtils.h"
#include "KSObjC.h"
#include "KSString.h"
#include "KSCrashMonitor_Signal.h"
#include "KSCrashMonitor_System.h"
#include "KSCrashMonitor_Zombie.h"
#include "KSCrashMonitor_AppState.h"
#include "KSCrashMonitorContext.h"
#include "KSSystemCapabilities.h"
#include "KSCrashMonitor_NSException.h"
//#define KSLogger_LocalLevel TRACE
#include "KSLogger.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>

typedef enum
{
    KSApplicationStateNone,
    KSApplicationStateDidBecomeActive,
    KSApplicationStateWillResignActiveActive,
    KSApplicationStateDidEnterBackground,
    KSApplicationStateWillEnterForeground,
    KSApplicationStateWillTerminate
} KSApplicationState;

// ============================================================================
#pragma mark - Globals -
// ============================================================================

/** True if KSCrash has been installed. */
static volatile bool g_installed = 0;

static bool g_shouldAddConsoleLogToReport = false;
static bool g_shouldPrintPreviousLog = false;
static char g_consoleLogPath[KSFU_MAX_PATH_LENGTH];
static KSCrashMonitorType g_monitoring = KSCrashMonitorTypeProductionSafeMinimal;
static char g_lastCrashReportFilePath[KSFU_MAX_PATH_LENGTH];
static KSReportWrittenCallback g_reportWrittenCallback;
static KSApplicationState g_lastApplicationState = KSApplicationStateNone;
static char* kscrash_binaryImagePathForAddress(uintptr_t ptr);

// ============================================================================
#pragma mark - Utility -
// ============================================================================

static void printPreviousLog(const char* filePath)
{
    char* data;
    int length;
    if(ksfu_readEntireFile(filePath, &data, &length, 0))
    {
        printf("\nvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv Previous Log vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv\n\n");
        printf("%s\n", data);
        free(data);
        printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n\n");
        fflush(stdout);
    }
}

static void notifyOfBeforeInstallationState(void)
{
    KSLOG_DEBUG("Notifying of pre-installation state");
    switch (g_lastApplicationState)
    {
        case KSApplicationStateDidBecomeActive:
            return kscrash_notifyAppActive(true);
        case KSApplicationStateWillResignActiveActive:
            return kscrash_notifyAppActive(false);
        case KSApplicationStateDidEnterBackground:
            return kscrash_notifyAppInForeground(false);
        case KSApplicationStateWillEnterForeground:
            return kscrash_notifyAppInForeground(true);
        case KSApplicationStateWillTerminate:
            return kscrash_notifyAppTerminate();
        default:
            return;
    }
}

// ============================================================================
#pragma mark - Callbacks -
// ============================================================================

/** Called when a crash occurs.
 *
 * This function gets passed as a callback to a crash handler.
 */
static void onCrash(struct KSCrash_MonitorContext* monitorContext)
{
    if (monitorContext->currentSnapshotUserReported == false) {
        KSLOG_DEBUG("Updating application state to note crash.");
        kscrashstate_notifyAppCrash();
    }
    monitorContext->consoleLogPath = g_shouldAddConsoleLogToReport ? g_consoleLogPath : NULL;

    if(monitorContext->crashedDuringCrashHandling)
    {
        kscrashreport_writeRecrashReport(monitorContext, g_lastCrashReportFilePath);
    }
    else
    {
        char crashReportFilePath[KSFU_MAX_PATH_LENGTH];
        int64_t reportID = kscrs_getNextCrashReport(crashReportFilePath);
        strncpy(g_lastCrashReportFilePath, crashReportFilePath, sizeof(g_lastCrashReportFilePath));
        kscrashreport_writeStandardReport(monitorContext, crashReportFilePath);

        if(g_reportWrittenCallback)
        {
            g_reportWrittenCallback(reportID);
        }
    }
}


// ============================================================================
#pragma mark - API -
// ============================================================================

bool kscrash_is_installed(void)
{
    return g_installed;
}

KSCrashMonitorType kscrash_install(const char* appName, const char* const installPath)
{
    KSLOG_DEBUG("Installing crash reporter.");

    if(g_installed)
    {
        KSLOG_DEBUG("Crash reporter already installed.");
        return kscm_getActiveMonitors();
    }
    g_installed = 1;

    char path[KSFU_MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/Reports", installPath);
    ksfu_makePath(path);
    kscrs_initialize(appName, path);

    snprintf(path, sizeof(path), "%s/Data", installPath);
    ksfu_makePath(path);
    snprintf(path, sizeof(path), "%s/Data/CrashState.json", installPath);
    kscrashstate_initialize(path);

    snprintf(g_consoleLogPath, sizeof(g_consoleLogPath), "%s/Data/ConsoleLog.txt", installPath);
    if(g_shouldPrintPreviousLog)
    {
        printPreviousLog(g_consoleLogPath);
    }
    kslog_setLogFilename(g_consoleLogPath, true);
    
    ksccd_init(60);

    kscm_setEventCallback(onCrash);
    KSCrashMonitorType monitors = kscrash_setMonitoring(g_monitoring);

    KSLOG_DEBUG("Installation complete.");

    notifyOfBeforeInstallationState();

    return monitors;
}

void kscrash_re_install()
{
    if(g_installed == 0)
    {
        KSLOG_DEBUG("install required to be called before re install");
        return;
    }
    
    forceExceptionHandlerToTopOfStack();
    
    KSLOG_DEBUG("Re Installation complete.");
}

// path must be null terminated
// the returned char must be free()d
char* lastComponentPath(const char* path)
{
    const char* last = NULL;
    const char *ptr = path;
    
    while(ptr != NULL && *ptr != 0)
    {
        if(*ptr == '/')
        {
            last = ptr + 1;
        }
        
        ptr++;
    }
    
    if(last && *last != 0)
    {
        size_t len = strlen(last);
        char* ret = (char*)malloc(sizeof(char) * (len + 1));
        memcpy(ret, last, len);
        ret[len] = 0;
        
        return ret;
    }
    
    return NULL;
}

struct KSCrash_SignalInfo* kscrash_getSignalInfo()
{
    int numPointers = 0;
    uintptr_t* pointers = kscm_getInstalledSignalFunctionPointers(&numPointers);
    
    if(numPointers == 0)
    {
        return NULL;
    }
    
    struct KSCrash_SignalInfo* list = malloc(sizeof(struct KSCrash_SignalInfo));
    KSCrash_initSignalInfo(list);
    
    struct KSCrash_SignalInfo* start = list;
    
    list->functionPointer = pointers[0];
    list->modulePath = kscrash_binaryImagePathForAddress(list->functionPointer);
    list->moduleName = lastComponentPath(list->modulePath);
    list->isEmbraceHandler = addressIsSignalHandler(list->functionPointer) ? 1 : 0;
    
    for(int i=1;i<numPointers;i++)
    {
        struct KSCrash_SignalInfo* c = malloc(sizeof(struct KSCrash_SignalInfo));
        KSCrash_initSignalInfo(c);
        
        c->functionPointer = pointers[i];
        c->modulePath = kscrash_binaryImagePathForAddress(c->functionPointer);
        c->moduleName = lastComponentPath(c->modulePath);
        c->isEmbraceHandler = addressIsSignalHandler(c->functionPointer) ? 1 : 0;
        
        list->next = c;
        list = c;
    }
    
    free(pointers);
    
    return start;
}

char* kscrash_binaryImagePathForAddress(uintptr_t ptr)
{
    Dl_info image_info;
    if (dladdr((const void *)ptr, &image_info) == 0) {
        KSLOG_WARN("Could not get info for binary image.");
        return NULL;
    }
    
    size_t len = strlen(image_info.dli_fname);
    char* ret = malloc(sizeof(char) * len + 1);
    strncpy(ret, image_info.dli_fname, len);
    ret[len] = 0;
    
    return ret;
}

KSCrashMonitorType kscrash_setMonitoring(KSCrashMonitorType monitors)
{
    if(g_installed)
    {
        kscm_setActiveMonitors(monitors);
        return kscm_getActiveMonitors();
    }
    
    // Return none because we are not installed yet and therefore not monitoring.
    return KSCrashMonitorTypeNone;
}

void kscrash_setUserInfoJSON(const char* const userInfoJSON)
{
    kscrashreport_setUserInfoJSON(userInfoJSON);
}

void kscrash_setDeadlockWatchdogInterval(double deadlockWatchdogInterval)
{
#if KSCRASH_HAS_OBJC
    kscm_setDeadlockHandlerWatchdogInterval(deadlockWatchdogInterval);
#endif
}

void kscrash_setSearchQueueNames(bool searchQueueNames)
{
    ksccd_setSearchQueueNames(searchQueueNames);
}

void kscrash_setIntrospectMemory(bool introspectMemory)
{
    kscrashreport_setIntrospectMemory(introspectMemory);
}

void kscrash_setDoNotIntrospectClasses(const char** doNotIntrospectClasses, int length)
{
    kscrashreport_setDoNotIntrospectClasses(doNotIntrospectClasses, length);
}

void kscrash_setCrashNotifyCallback(const KSReportWriteCallback onCrashNotify)
{
    kscrashreport_setUserSectionWriteCallback(onCrashNotify);
}

void kscrash_setReportWrittenCallback(const KSReportWrittenCallback onReportWrittenNotify)
{
    g_reportWrittenCallback = onReportWrittenNotify;
}

void kscrash_setAddConsoleLogToReport(bool shouldAddConsoleLogToReport)
{
    g_shouldAddConsoleLogToReport = shouldAddConsoleLogToReport;
}

void kscrash_setPrintPreviousLog(bool shouldPrintPreviousLog)
{
    g_shouldPrintPreviousLog = shouldPrintPreviousLog;
}

void kscrash_setMaxReportCount(int maxReportCount)
{
    kscrs_setMaxReportCount(maxReportCount);
}

void kscrash_reportUserException(const char* name,
                                 const char* reason,
                                 const char* language,
                                 const char* lineOfCode,
                                 const char* stackTrace,
                                 bool logAllThreads,
                                 bool terminateProgram)
{
    kscm_reportUserException(name,
                             reason,
                             language,
                             lineOfCode,
                             stackTrace,
                             logAllThreads,
                             terminateProgram);
    if(g_shouldAddConsoleLogToReport)
    {
        kslog_clearLogFile();
    }
}

void enableSwapCxaThrow(void)
{
    kscm_enableSwapCxaThrow();
}

void kscrash_notifyObjCLoad(void)
{
    kscrashstate_notifyObjCLoad();
}

void kscrash_notifyAppActive(bool isActive)
{
    if (g_installed)
    {
        kscrashstate_notifyAppActive(isActive);
    }
    g_lastApplicationState = isActive
        ? KSApplicationStateDidBecomeActive
        : KSApplicationStateWillResignActiveActive;
}

void kscrash_notifyAppInForeground(bool isInForeground)
{
    if (g_installed)
    {
        kscrashstate_notifyAppInForeground(isInForeground);
    }
    g_lastApplicationState = isInForeground
        ? KSApplicationStateWillEnterForeground
        : KSApplicationStateDidEnterBackground;
}

void kscrash_notifyAppTerminate(void)
{
    if (g_installed)
    {
        kscrashstate_notifyAppTerminate();
    }
    g_lastApplicationState = KSApplicationStateWillTerminate;
}

void kscrash_notifyAppCrash(void)
{
    kscrashstate_notifyAppCrash();
}

int kscrash_getReportCount()
{
    return kscrs_getReportCount();
}

int kscrash_getReportIDs(int64_t* reportIDs, int count)
{
    return kscrs_getReportIDs(reportIDs, count);
}

char* kscrash_readReport(int64_t reportID)
{
    if(reportID <= 0)
    {
        KSLOG_ERROR("Report ID was %" PRIx64, reportID);
        return NULL;
    }

    char* rawReport = kscrs_readReport(reportID);
    if(rawReport == NULL)
    {
        KSLOG_ERROR("Failed to load report ID %" PRIx64, reportID);
        return NULL;
    }

    char* fixedReport = kscrf_fixupCrashReport(rawReport);
    if(fixedReport == NULL)
    {
        KSLOG_ERROR("Failed to fixup report ID %" PRIx64, reportID);
    }

    free(rawReport);
    return fixedReport;
}

int64_t kscrash_addUserReport(const char* report, int reportLength)
{
    return kscrs_addUserReport(report, reportLength);
}

void kscrash_deleteAllReports()
{
    kscrs_deleteAllReports();
}

void kscrash_deleteReportWithID(int64_t reportID)
{
    kscrs_deleteReportWithID(reportID);
}
