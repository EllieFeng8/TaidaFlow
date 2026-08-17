// ProxyRequestRelay 將已註冊 Proxy 的具型別、非 Q_PROPERTY NOTIFY signal
// 轉成 ProxyMirror transport 使用的通用 signature + QVariantList event。
// 實際 QObject::connect 程式碼由建置系統依各 Proxy 的 moc JSON 自動產生；
// 新增 Proxy 類別時只需要在 CMake 呼叫 wasm_mirror_register_proxy()。
#pragma once

#include <functional>

#include <QString>
#include <QVariantList>

class QObject;

using ProxyEventHandler =
    std::function<void(const QString &signalSignature,
                       const QVariantList &arguments)>;

// 將 proxy 具體類別中所有非 Q_PROPERTY NOTIFY signal 接到 handler。
// 連線生命週期由 lifetimeContext 管理；context 銷毀後 Qt 會自動中斷 relay。
//
// proxy 的具體類別必須先用 wasm_mirror_register_proxy() 註冊。若類別未註冊、
// handler 為空或產生的型別分派失敗，回傳 false 並填入 errorMessage。
bool connectProxyMirrorEvents(QObject &proxy,
                              QObject &lifetimeContext,
                              ProxyEventHandler handler,
                              QString *errorMessage = nullptr);
