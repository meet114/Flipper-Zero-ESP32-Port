#pragma once

#include "wifi_marauder_script.h"

typedef enum {
    WifiMarauderScriptWorkerStatusSuccess = 0,
    WifiMarauderScriptWorkerStatusInvalidScript = 1,
    WifiMarauderScriptWorkerStatusForceExit = 2
} WifiMarauderScriptWorkerStatus;

typedef struct WifiMarauderScriptWorker {
    WifiMarauderScript* script;
    FuriThread* worker_thread;
    WifiMarauderUart* uart;
    void (*callback_start)(void*);
    void (*callback_stage)(WifiMarauderScriptStage*, void*);
    void* context;
    bool is_running;
} WifiMarauderScriptWorker;

WifiMarauderScriptWorker* wifi_marauder_script_worker_alloc();

bool wifi_marauder_script_worker_start(
    WifiMarauderScriptWorker* instance,
    WifiMarauderScript* script);

void wifi_marauder_script_worker_free(WifiMarauderScriptWorker* script);
